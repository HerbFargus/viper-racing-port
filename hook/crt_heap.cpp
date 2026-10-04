// crt_heap.cpp -- M3 stage LIBC, group B: the game's C runtime heap, rewritten. LIBC objects malloc, free, realloc,
// calloc, msize. The single-threaded 1997 LIBC.lib has no small-block heap of its own: every block is a Win32 heap
// block of _crtheap (0x5d6e10, made by the deferred _heap_init), through the game's import slots. The new handler
// (_callnewh, handler.obj, deferred) is called by address when _newmode is set (it never is: the game doesn't call
// _set_new_mode).
//
// Faithful to the v1.0 disassembly: realloc and calloc read their HeapReAlloc / HeapAlloc slot once, at entry, and
// retry through that pointer after the new handler; malloc, _nh_malloc and _heap_alloc each read theirs at the call.
// Requests above 0xffffffe0 bytes fail without calling Windows; malloc(0) asks for 1 byte, calloc of 0 bytes too
// (and calloc's num * size is the low 32 bits of the product, unchecked); realloc(p, 0) frees p and returns 0.
//
// These run before WinMain too (the DLL is hooked in before the C runtime starts: _setargv, _setenvp, __initstdio's
// calloc, _ioinit's are the first calls). All of them allocate or free, so their footprints are replay_only: a
// replay is their check (and test/world_crt_io.cpp offline, by the heap's layout after random sequences).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include "port.h"
#include "crt_types.h"

namespace crt_heap {   // (file-local helpers; one namespace per file so a harness can include all four)

#define IAT(slot, fn) (*(decltype(&fn) volatile*)(uintptr_t)(slot))
#define kHeapAlloc   IAT(0x005d7498, HeapAlloc)
#define kHeapFree    IAT(0x005d74a0, HeapFree)
#define kHeapSize    IAT(0x005d7590, HeapSize)
#define kHeapReAlloc_slot 0x005d7588u
#define kHeapAlloc_slot   0x005d7498u
typedef LPVOID(WINAPI* HeapReAlloc_t)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef LPVOID(WINAPI* HeapAlloc_t)(HANDLE, DWORD, SIZE_T);

template <typename R, typename... A> static __forceinline R gcall(uint32_t fn, A... a) {
    typedef R(__cdecl * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}
enum : uint32_t { F_malloc = 0x004d1de0, F_nh_malloc = 0x004d1e00, F_heap_alloc = 0x004d1e40, F_free = 0x004d58b0,
                  F_callnewh = 0x004d64e0 };

// _heap_alloc (0x4d1e40)
static void* __cdecl c_heap_alloc(uint32_t size) { return kHeapAlloc(crt_crtheap, 0, size); }
static void fp_heap_alloc(Footprint& f, uint32_t) { f.replay_only = "allocates (HeapAlloc)"; }
PORT_FN(0x004d1e40, "heap_alloc", c_heap_alloc, fp_heap_alloc)

// _nh_malloc (0x4d1e00): 0 asks for 1; past 0xffffffe0 fails; the new handler (nh_mode) retries
static void* __cdecl c_nh_malloc(uint32_t size, int32_t nh_mode) {
    if (size > 0xffffffe0u) return 0;
    if (size == 0) size = 1;
    for (;;) {
        void* p = gcall<void*>(F_heap_alloc, size);
        if (p || !nh_mode) return p;
        if (!gcall<int32_t>(F_callnewh, size)) return 0;
    }
}
static void fp_nh_malloc(Footprint& f, uint32_t, int32_t) { f.replay_only = "allocates"; }
PORT_FN(0x004d1e00, "nh_malloc", c_nh_malloc, fp_nh_malloc)

// malloc (0x4d1de0)
static void* __cdecl c_malloc(uint32_t size) { return gcall<void*>(F_nh_malloc, size, crt_newmode); }
static void fp_malloc(Footprint& f, uint32_t) { f.replay_only = "allocates"; }
PORT_FN(0x004d1de0, "malloc", c_malloc, fp_malloc)

// free (0x4d58b0)
static void __cdecl c_free(void* p) {
    if (p) kHeapFree(crt_crtheap, 0, p);
}
static void fp_free(Footprint& f, void*) { f.replay_only = "frees (HeapFree)"; }
PORT_FN(0x004d58b0, "free", c_free, fp_free)

// _msize (0x4d1dc0)
static uint32_t __cdecl c_msize(void* p) { return (uint32_t)kHeapSize(crt_crtheap, 0, p); }
static void fp_msize(Footprint&, void*) {}                                  // writes nothing
PORT_FN(0x004d1dc0, "msize", c_msize, fp_msize)

// realloc (0x4d1d50): 0 -> malloc; size 0 -> free, 0; else HeapReAlloc in place or moved
static void* __cdecl c_realloc(void* p, uint32_t size) {
    if (!p) return gcall<void*>(F_malloc, size);
    if (!size) {
        gcall<void>(F_free, p);
        return 0;
    }
    const HeapReAlloc_t re = *(HeapReAlloc_t volatile*)(uintptr_t)kHeapReAlloc_slot;
    void* heap = crt_crtheap;
    for (;;) {
        void* q = size > 0xffffffe0u ? 0 : re(heap, 0, p, size);
        if (q || !crt_newmode) return q;
        const int32_t again = gcall<int32_t>(F_callnewh, size);
        heap = crt_crtheap;
        if (!again) return 0;
    }
}
static void fp_realloc(Footprint& f, void*, uint32_t) { f.replay_only = "allocates and frees"; }
PORT_FN(0x004d1d50, "realloc", c_realloc, fp_realloc)

// calloc (0x4d7170): num * size (32 bits), zeroed by the heap (HEAP_ZERO_MEMORY)
static void* __cdecl c_calloc(uint32_t num, uint32_t size) {
    uint32_t n = num * size;
    if (!n) n = 1;
    const HeapAlloc_t al = *(HeapAlloc_t volatile*)(uintptr_t)kHeapAlloc_slot;
    void* heap = crt_crtheap;
    for (;;) {
        void* q = n > 0xffffffe0u ? 0 : al(heap, HEAP_ZERO_MEMORY, n);
        if (q || !crt_newmode) return q;
        const int32_t again = gcall<int32_t>(F_callnewh, n);
        heap = crt_crtheap;
        if (!again) return 0;
    }
}
static void fp_calloc(Footprint& f, uint32_t, uint32_t) { f.replay_only = "allocates"; }
PORT_FN(0x004d7170, "calloc", c_calloc, fp_calloc)

}   // namespace crt_heap
