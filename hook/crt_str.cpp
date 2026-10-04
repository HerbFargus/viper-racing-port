// crt_str.cpp -- M3 stage LIBC, group B: the game's C runtime's strings, sorting, ctype and multibyte code, rewritten.
// LIBC objects: strchr, strncmp, strrchr, strncpy, strncat, strstr, memmove, stricmp, strnicmp (the hand-written
// assembly ones: rep scas / cmps / movs, std ... cld), qsort, bsearch, _ctype (isdigit, isspace), isctype, tolower,
// toupper, ismbbyte, mbctype (_setmbcp and its helpers, __initmbctable), mbtoupr, mbtowc, wctomb, aw_map
// (__crtLCMapStringA), aw_str (__crtGetStringTypeA).
//
// Written from the v1.0 disassembly (tools/disasm.py), faithful: every call in the original's order and by its v1.0
// address (this group's own too, so a hooked rewrite is what runs), Windows through the game's import slots (read
// where the original reads them: __crtLCMapStringA and __crtGetStringTypeA load the A function's slot once, at entry),
// the CRT's state at its v1.0 address (crt_types.h). What the assembly routines compute is reproduced exactly; how
// they move bytes (dwords versus bytes, forwards versus backwards) is not observable apart from the result, which is
// the same for every input, overlapping ones included (memmove's backward path even reads the caller's edx for its
// first alignment step, which changes only how the bytes are grouped).
//
// The game runs in the "C" locale (nothing calls setlocale: __lc_handle[LC_CTYPE] stays 0) with the single-byte code
// page __initmbctable picked up from GetACP, so tolower / toupper / stricmp / strnicmp take their ASCII paths and
// mbtowc / wctomb their one-byte ones. The locale paths are ported anyway, with their quirks:
//   * stricmp's locale path keeps eax across iterations and only reloads its low byte: a tolower that returned a value
//     past 0xff leaves upper bits that the "equal and zero" exit returns (a non-zero "equal").
//   * strnicmp's locale path returns the opposite sign (+1 where s1 < s2): it compares tolower(s1) with tolower(s2)
//     and maps "below" to 1.
//   * isctype / tolower / toupper hand the API a local buffer laid out as the original's frame: GetStringTypeA
//     writes one word per source character, so a two-byte character's second word lands on the source buffer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "crt_types.h"

namespace crt_str {   // (file-local helpers; one namespace per file so a harness can include all four)

// ---- the game's imports and functions -------------------------------------------------------------------------------
#define IAT(slot, fn) (*(decltype(&fn) volatile*)(uintptr_t)(slot))
#define kLCMapStringA_slot        0x005d75a4u
#define kLCMapStringW             IAT(0x005d7584, LCMapStringW)
#define kMultiByteToWideChar      IAT(0x005d75c4, MultiByteToWideChar)
#define kWideCharToMultiByte      IAT(0x005d75d4, WideCharToMultiByte)
#define kGetStringTypeA_slot      0x005d7580u
#define kGetStringTypeW           IAT(0x005d75fc, GetStringTypeW)
#define kGetCPInfo                IAT(0x005d75d8, GetCPInfo)
#define kGetOEMCP                 IAT(0x005d75b8, GetOEMCP)
#define kGetACP                   IAT(0x005d75dc, GetACP)
typedef int(WINAPI* LCMapStringA_t)(LCID, DWORD, LPCSTR, int, LPSTR, int);
typedef BOOL(WINAPI* GetStringTypeA_t)(LCID, DWORD, LPCSTR, int, LPWORD);

template <typename R, typename... A> static __forceinline R gcall(uint32_t fn, A... a) {
    typedef R(__cdecl * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}
typedef int(__cdecl* Cmp)(const void*, const void*);

enum : uint32_t {
    F_malloc = 0x004d1de0, F_free = 0x004d58b0, F_calloc = 0x004d7170,
    F_swap = 0x004cf320, F_shortsort = 0x004cf2c0, F_isctype = 0x004d47a0, F_tolower = 0x004cfd50,
    F_crtLCMapStringA = 0x004d4ae0, F_strncnt = 0x004d4d10, F_crtGetStringTypeA = 0x004d68f0,
    F_x_ismbbtype = 0x004d3a80, F_getSystemCP = 0x004d41a0, F_CPtoLCID = 0x004d41f0, F_setSBCS = 0x004d4250,
    F_setmbcp = 0x004d3fc0,
};
#define G32(va) CRT_G(uint32_t, va)
#define G8(va) CRT_G(uint8_t, va)
static __forceinline uint16_t pctype_at(int32_t c) { return crt_pctype[c]; }   // _pctype[c] (c may be -1)

// =====================================================================================================================
// the assembly string routines
// =====================================================================================================================
// strchr (0x4ce0c0): the first c (a byte; 0 finds the terminator)
static char* __cdecl c_strchr(const char* s, int32_t c) {
    const uint8_t ch = (uint8_t)c;
    for (;; s++) {
        if ((uint8_t)*s == ch) return (char*)s;
        if (!*s) return 0;
    }
}
// (the functions returning a pointer into their arguments aren't marked pure: fuzz.exe compares return values as
// they are, which differ between its two arenas; test/world_crt_io.cpp fuzzes them)
static void fp_strchr(Footprint&, const char*, int32_t) {}
PORT_FN(0x004ce0c0, "strchr", c_strchr, fp_strchr)

// strncmp (0x4ce0e0): k = the bytes of s1 up to and including its NUL, at most n; compares k bytes (stopping at the
// first difference) and returns the sign of the last pair compared, unsigned: -1, 0 or 1
static int32_t __cdecl c_strncmp(const char* s1, const char* s2, uint32_t n) {
    if (!n) return 0;
    uint32_t k = 0;
    while (k < n)
        if (s1[k++] == 0) break;
    uint32_t i = 0;
    uint8_t a, b;
    do {
        a = (uint8_t)s2[i];
        b = (uint8_t)s1[i];
        i++;
    } while (i < k && a == b);
    return a > b ? -1 : a == b ? 0 : 1;
}
static void fp_strncmp(Footprint& f, const char*, const char*, uint32_t) { f.pure = true; }
PORT_FN(0x004ce0e0, "strncmp", c_strncmp, fp_strncmp)

// strrchr (0x4cf130): from the terminator backwards (c = 0 finds the terminator)
static char* __cdecl c_strrchr(const char* s, int32_t c) {
    const uint8_t ch = (uint8_t)c;
    const char* p = s;
    while (*p) p++;
    for (;; p--) {
        if ((uint8_t)*p == ch) return (char*)p;
        if (p == s) return 0;
    }
}
static void fp_strrchr(Footprint&, const char*, int32_t) {}
PORT_FN(0x004cf130, "strrchr", c_strrchr, fp_strrchr)

// strncpy (0x4cf3a0): up to n characters, stopping at the NUL, then zeros to n
static char* __cdecl c_strncpy(char* dst, const char* src, uint32_t n) {
    char* d = dst;
    uint32_t c = n;
    if (c) {
        do {
            const char ch = *src++;
            if (!ch) break;
            *d++ = ch;
        } while (--c);
    }
    for (; c; c--) *d++ = 0;
    return dst;
}
static void fp_strncpy(Footprint& f, char* dst, const char*, uint32_t n) { f.add(dst, n, "strncpy dst"); }
PORT_FN(0x004cf3a0, "strncpy", c_strncpy, fp_strncpy)

// memmove (0x4cf400): forwards unless dst lies inside (src, src + n) (the comparison in 32-bit addresses)
static void* __cdecl c_memmove(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;                     // in the original's direction (the result is memmove's either way)
    const uint8_t* s = (const uint8_t*)src;
    if ((uint32_t)(uintptr_t)dst <= (uint32_t)(uintptr_t)src ||
        (uint32_t)(uintptr_t)dst >= (uint32_t)(uintptr_t)src + n) {
        for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (uint32_t i = n; i--;) d[i] = s[i];
    }
    return dst;
}
static void fp_memmove(Footprint& f, void* dst, const void*, uint32_t n) { f.add(dst, n, "memmove dst"); }
PORT_FN(0x004cf400, "memmove", c_memmove, fp_memmove)

// strstr (0x4cf9a0): an empty pattern matches at s
static char* __cdecl c_strstr(const char* s, const char* pat) {
    const char* start = s;
    const char* p = pat;
    for (;;) {
        const char a = *p;
        if (!a) return (char*)start;
        const char b = *s;
        if (!b) return 0;
        if (a == b) {
            p++;
            s++;
        } else {
            p = pat;
            s = ++start;
        }
    }
}
static void fp_strstr(Footprint&, const char*, const char*) {}
PORT_FN(0x004cf9a0, "strstr", c_strstr, fp_strstr)

// strncat (0x4d8380): min(n, strlen(src)) characters on the end of dst, then a NUL (n = 0: the second scan doesn't
// run, and the flags it would leave are dec edi's, not zero: nothing copied, the NUL rewritten)
static char* __cdecl c_strncat(char* dst, const char* src, uint32_t n) {
    char* e = dst;
    while (*e) e++;
    uint32_t cnt = 0;
    while (cnt < n && src[cnt]) cnt++;              // the NUL found at cnt, or n scanned without one
    const char* s = src;
    char* d = e;
    for (uint32_t i = 0; i < cnt; i++) *d++ = *s++;
    *d = 0;
    return dst;
}
static void fp_strncat(Footprint& f, char* dst, const char* src, uint32_t n) {
    uint32_t a = 0, b = 0;
    while (dst[a]) a++;
    while (b < n && src[b]) b++;
    f.add(dst, a + b + 1, "strncat dst");
}
PORT_FN(0x004d8380, "strncat", c_strncat, fp_strncat)

// _stricmp (0x4da350): the "C" locale folds 'A'..'Z' and returns -1 / 0 / 1; otherwise through tolower (by address),
// eax kept across iterations with only its low byte reloaded (see the top)
static int32_t __cdecl c_stricmp(const char* s1, const char* s2) {
    const uint8_t* a1 = (const uint8_t*)s1;
    const uint8_t* a2 = (const uint8_t*)s2;
    if (crt_lc_ctype == 0) {
        uint8_t al = 0xff, ah;
        for (;;) {
            if (!al) return (int32_t)(int8_t)al;
            al = *a2++;
            ah = *a1++;
            if (ah == al) continue;
            al = (uint8_t)(al - 0x41) < 0x1a ? (uint8_t)(al + 0x20) : al;   // s2's
            const uint8_t t = al;
            al = ah;
            ah = t;
            al = (uint8_t)(al - 0x41) < 0x1a ? (uint8_t)(al + 0x20) : al;   // s1's
            if (al == ah) continue;
            return al < ah ? -1 : 1;
        }
    }
    uint32_t eax = 0xff, ebx = 0;
    for (;;) {
        if ((uint8_t)eax == 0) return (int32_t)eax;
        eax = (eax & 0xffffff00u) | *a2++;
        ebx = (ebx & 0xffffff00u) | *a1++;
        if ((uint8_t)eax == (uint8_t)ebx) continue;
        const uint32_t lb = gcall<uint32_t>(F_tolower, ebx);
        const uint32_t la = gcall<uint32_t>(F_tolower, eax);
        ebx = lb;
        eax = la;
        if ((uint8_t)ebx == (uint8_t)eax) continue;
        return (uint8_t)ebx < (uint8_t)eax ? -1 : 1;
    }
}
static void fp_stricmp(Footprint& f, const char*, const char*) {
    f.add((void*)(uintptr_t)CRT_AWMAP_FUSE_VA, 4, "aw_map f_use");     // (tolower's locale path)
}
PORT_FN(0x004da350, "stricmp", c_stricmp, fp_stricmp)

// _strnicmp (0x4da3e0): at most n characters; the "C" locale stops at either NUL without folding it; the locale path
// returns the opposite sign (see the top)
static int32_t __cdecl c_strnicmp(const char* s1, const char* s2, uint32_t n) {
    if (!n) return 0;
    const uint8_t* a1 = (const uint8_t*)s1;
    const uint8_t* a2 = (const uint8_t*)s2;
    if (crt_lc_ctype == 0) {
        uint8_t ah, al;
        for (;;) {
            ah = *a1;
            al = *a2;
            if (!ah || !al) break;
            a1++;
            a2++;
            if (ah >= 0x41 && ah <= 0x5a) ah = (uint8_t)(ah + 0x20);
            if (al >= 0x41 && al <= 0x5a) al = (uint8_t)(al + 0x20);
            if (ah != al) return ah < al ? -1 : 1;
            if (--n == 0) break;
        }
        return ah == al ? 0 : ah < al ? -1 : 1;
    }
    uint32_t eax = 0, ebx = 0;
    for (;;) {
        eax = (eax & 0xffffff00u) | *a1;
        ebx = (ebx & 0xffffff00u) | *a2;
        if (eax == 0 || ebx == 0) break;
        a1++;
        a2++;
        const uint32_t lb = gcall<uint32_t>(F_tolower, ebx);
        const uint32_t la = gcall<uint32_t>(F_tolower, eax);
        ebx = lb;
        eax = la;
        if (eax != ebx) return eax < ebx ? 1 : -1;
        if (--n == 0) break;
    }
    return eax == ebx ? 0 : eax < ebx ? 1 : -1;
}
static void fp_strnicmp(Footprint& f, const char*, const char*, uint32_t) {
    f.add((void*)(uintptr_t)CRT_AWMAP_FUSE_VA, 4, "aw_map f_use");
}
PORT_FN(0x004da3e0, "strnicmp", c_strnicmp, fp_strnicmp)

// =====================================================================================================================
// qsort.obj, bsearch.obj
// =====================================================================================================================
// swap (0x4cf320): width bytes, unless a == b
static void __cdecl c_swap(char* a, char* b, uint32_t width) {
    if (a == b) return;
    while (width--) {
        const char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}
static void fp_swap(Footprint& f, char* a, char* b, uint32_t w) { f.pure = true; f.add(a, w, "swap a"); f.add(b, w, "swap b"); }
PORT_FN(0x004cf320, "swap", c_swap, fp_swap)

// shortsort (0x4cf2c0): selection sort from the top: the last of the greatest (comp(p, max) > 0 moves it) to hi
static void __cdecl c_shortsort(char* lo, char* hi, uint32_t width, void* comp) {
    while (hi > lo) {
        char* max = lo;
        for (char* p = lo + width; p <= hi; p += width)
            if (((Cmp)comp)(p, max) > 0) max = p;
        gcall<void>(F_swap, max, hi, width);
        hi -= width;
    }
}
static void fp_shortsort(Footprint& f, char* lo, char* hi, uint32_t width, void*) {
    if (hi >= lo) f.add(lo, (uint32_t)(hi - lo) + width, "shortsort array");
}
PORT_FN(0x004cf2c0, "shortsort", c_shortsort, fp_shortsort)

// qsort (0x4cf160): the 1997 CRT's: middle element swapped to lo as the pivot, partition, the smaller side first on an
// explicit 30-deep stack, shortsort at 8 elements or fewer. The order of equal elements is this algorithm's.
static void __cdecl c_qsort(void* base, uint32_t num, uint32_t width, void* comp) {
    char* lostk[30];
    char* histk[30];
    if (num < 2 || width == 0) return;
    int32_t stkptr = 0;
    char* lo = (char*)base;
    char* hi = (char*)base + width * (num - 1);
    for (;;) {
        const uint32_t size = (uint32_t)(hi - lo) / width + 1;
        if (size <= 8) {
            gcall<void>(F_shortsort, lo, hi, width, comp);
        } else {
            char* mid = lo + (size >> 1) * width;
            gcall<void>(F_swap, mid, lo, width);
            char* loguy = lo;
            char* higuy = hi + width;
            for (;;) {
                do loguy += width;
                while (loguy <= hi && ((Cmp)comp)(loguy, lo) <= 0);
                do higuy -= width;
                while (higuy > lo && ((Cmp)comp)(higuy, lo) >= 0);
                if (higuy < loguy) break;
                gcall<void>(F_swap, loguy, higuy, width);
            }
            gcall<void>(F_swap, lo, higuy, width);
            if ((int32_t)((uint32_t)(uintptr_t)higuy - (uint32_t)(uintptr_t)lo - 1) >=
                (int32_t)((uint32_t)(uintptr_t)hi - (uint32_t)(uintptr_t)loguy)) {
                if (lo + width < higuy) {
                    lostk[stkptr] = lo;
                    histk[stkptr] = higuy - width;
                    ++stkptr;
                }
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi) {
                    lostk[stkptr] = loguy;
                    histk[stkptr] = hi;
                    ++stkptr;
                }
                if (lo + width < higuy) {
                    hi = higuy - width;
                    continue;
                }
            }
        }
        if (--stkptr < 0) return;
        lo = lostk[stkptr];
        hi = histk[stkptr];
    }
}
static void fp_qsort(Footprint& f, void* base, uint32_t num, uint32_t width, void*) {
    f.add(base, num * width, "qsort array");
}
PORT_FN(0x004cf160, "qsort", c_qsort, fp_qsort)

// bsearch (0x4cfca0)
static void* __cdecl c_bsearch(const void* key, const void* base, uint32_t num, uint32_t width, void* comp) {
    char* lo = (char*)base;
    char* hi = (char*)base + (num - 1) * width;
    while (lo <= hi) {
        const uint32_t half = num >> 1;
        if (half) {
            const uint32_t odd = num & 1;
            char* mid = lo + (odd ? half : half - 1) * width;
            const int32_t r = ((Cmp)comp)(key, mid);
            if (!r) return mid;
            if (r < 0) {
                hi = mid - width;
                num = odd ? half : half - 1;
            } else {
                lo = mid + width;
                num = half;
            }
        } else {
            if (!num) return 0;
            return ((Cmp)comp)(key, lo) ? 0 : lo;
        }
    }
    return 0;
}
static void fp_bsearch(Footprint&, const void*, const void*, uint32_t, uint32_t, void*) {}
PORT_FN(0x004cfca0, "bsearch", c_bsearch, fp_bsearch)

// =====================================================================================================================
// ctype: _ctype.obj, isctype.obj, tolower.obj, toupper.obj
// =====================================================================================================================
// isdigit (0x4cf760) / isspace (0x4cf790): _pctype, or _isctype when MB_CUR_MAX > 1
static int32_t __cdecl c_isdigit(int32_t c) {
    if (crt_mb_cur_max > 1) return gcall<int32_t>(F_isctype, c, 4);
    return pctype_at(c) & 4;
}
static int32_t __cdecl c_isspace(int32_t c) {
    if (crt_mb_cur_max > 1) return gcall<int32_t>(F_isctype, c, 8);
    return pctype_at(c) & 8;
}
static void fp_isx(Footprint& f, int32_t) { f.add((void*)(uintptr_t)CRT_AWSTR_FUSE_VA, 4, "aw_str f_use"); }
PORT_FN(0x004cf760, "isdigit", c_isdigit, fp_isx)
PORT_FN(0x004cf790, "isspace", c_isspace, fp_isx)

// _isctype (0x4d47a0): -1..255 from _pctype; otherwise GetStringType on the one or two bytes. The frame as the
// original's: chartype at +2, the source at +4 (a two-byte source gets two words written from +2)
static int32_t __cdecl c_isctype(int32_t c, int32_t mask) {
    if ((uint32_t)(c + 1) <= 0x100) return pctype_at(c) & mask;
    alignas(4) uint8_t fr[8] = {};                  // esp+4 .. esp+0xb
    const uint8_t hi = (uint8_t)((uint32_t)c >> 8);
    int32_t size;
    if (pctype_at(hi) & 0x8000) {
        fr[4] = hi;
        fr[6] = 0;
        fr[5] = (uint8_t)c;
        size = 2;
    } else {
        fr[4] = (uint8_t)c;
        fr[5] = 0;
        size = 1;
    }
    if (!gcall<int32_t>(F_crtGetStringTypeA, 1, (const char*)&fr[4], size, (uint16_t*)&fr[2], 0, 0)) return 0;
    uint32_t w;
    memcpy(&w, &fr[2], 4);
    return (int32_t)(w & 0xffff & (uint32_t)mask);
}
static void fp_isctype(Footprint& f, int32_t, int32_t) { f.add((void*)(uintptr_t)CRT_AWSTR_FUSE_VA, 4, "aw_str f_use"); }
PORT_FN(0x004d47a0, "isctype", c_isctype, fp_isctype)

// tolower (0x4cfd50) / toupper (0x4d4df0): the "C" locale folds ASCII; otherwise LCMapString (frame: the result at +0,
// the source at +4)
static int32_t lc_map_case(int32_t c, uint32_t flags) {
    alignas(4) uint8_t fr[8] = {};                  // esp+4 .. esp+0xb
    const uint8_t hi = (uint8_t)((uint32_t)c >> 8);
    int32_t size;
    if (crt_pctype[hi] & 0x8000) {
        fr[4] = hi;
        fr[6] = 0;
        fr[5] = (uint8_t)c;
        size = 2;
    } else {
        fr[4] = (uint8_t)c;
        fr[5] = 0;
        size = 1;
    }
    const int32_t r = gcall<int32_t>(F_crtLCMapStringA, crt_lc_ctype, flags, (const char*)&fr[4], size, (char*)&fr[0], 3, 0);
    if (!r) return c;
    if (r == 1) return fr[0];
    return (int32_t)(((uint32_t)fr[1] << 8) | fr[0]);
}
static int32_t __cdecl c_tolower(int32_t c) {
    if (crt_lc_ctype == 0) {
        if (c >= 0x41 && c <= 0x5a) c += 0x20;
        return c;
    }
    if (c < 0x100) {
        const int32_t up = crt_mb_cur_max > 1 ? gcall<int32_t>(F_isctype, c, 1) : pctype_at(c) & 1;
        if (!up) return c;
    }
    return lc_map_case(c, 0x100);
}
static int32_t __cdecl c_toupper(int32_t c) {
    if (crt_lc_ctype == 0) {
        if (c >= 0x61 && c <= 0x7a) c -= 0x20;
        return c;
    }
    if (c < 0x100) {
        const int32_t lo = crt_mb_cur_max > 1 ? gcall<int32_t>(F_isctype, c, 2) : pctype_at(c) & 2;
        if (!lo) return c;
    }
    return lc_map_case(c, 0x200);
}
static void fp_case(Footprint& f, int32_t) {
    f.add((void*)(uintptr_t)CRT_AWMAP_FUSE_VA, 4, "aw_map f_use");
    f.add((void*)(uintptr_t)CRT_AWSTR_FUSE_VA, 4, "aw_str f_use");
}
PORT_FN(0x004cfd50, "tolower", c_tolower, fp_case)
PORT_FN(0x004d4df0, "toupper", c_toupper, fp_case)

// =====================================================================================================================
// multibyte: ismbbyte.obj, mbctype.obj, mbtoupr.obj, mbtowc.obj, wctomb.obj
// =====================================================================================================================
// _ismbbtype (0x4d3a80): _mbctype[c + 1] & kmask, or (cmask) _ctype[c + 1] & cmask -- the table itself, not _pctype
static int32_t __cdecl c_x_ismbbtype(uint32_t c, int32_t cmask, int32_t kmask) {
    const uint32_t b = (uint8_t)c;
    if ((uint32_t)kmask & G8(CRT_MBCTYPE_VA + 1 + b)) return 1;
    uint32_t v = 0;
    if (cmask) v = (uint32_t)CRT_G(uint16_t, 0x00503082u + b * 2) & (uint32_t)cmask;
    return v ? 1 : 0;
}
static void fp_x_ismbbtype(Footprint&, uint32_t, int32_t, int32_t) {}
PORT_FN(0x004d3a80, "x_ismbbtype", c_x_ismbbtype, fp_x_ismbbtype)

// _ismbblead (0x4d3a60)
static int32_t __cdecl c_ismbblead(uint32_t c) { return gcall<int32_t>(F_x_ismbbtype, c, 0, 4); }
static void fp_ismbblead(Footprint&, uint32_t) {}
PORT_FN(0x004d3a60, "ismbblead", c_ismbblead, fp_ismbblead)

// getSystemCP (0x4d41a0): -2 the OEM code page, -3 the ANSI one, -4 the locale's (each sets fSystemSet)
static uint32_t __cdecl c_getSystemCP(int32_t cp) {
    G32(CRT_FSYSTEMSET_VA) = 0;
    if (cp == -2) {
        G32(CRT_FSYSTEMSET_VA) = 1;
        return kGetOEMCP();
    }
    if (cp == -3) {
        G32(CRT_FSYSTEMSET_VA) = 1;
        return kGetACP();
    }
    if (cp == -4) {
        G32(CRT_FSYSTEMSET_VA) = 1;
        return crt_lc_codepage;
    }
    return (uint32_t)cp;
}
static void fp_getSystemCP(Footprint& f, int32_t) { f.add((void*)(uintptr_t)CRT_FSYSTEMSET_VA, 4, "fSystemSet"); }
PORT_FN(0x004d41a0, "getSystemCP", c_getSystemCP, fp_getSystemCP)

// CPtoLCID (0x4d41f0): the four East Asian code pages' locales
static uint32_t __cdecl c_CPtoLCID(int32_t cp) {
    switch (cp) {
    case 932: return 0x411;
    case 936: return 0x804;
    case 949: return 0x412;
    case 950: return 0x404;
    }
    return 0;
}
static void fp_CPtoLCID(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x004d41f0, "CPtoLCID", c_CPtoLCID, fp_CPtoLCID)

static void mbctype_clear() {
    for (uint32_t i = 0; i < 0x101; i++) G8(CRT_MBCTYPE_VA + i) = 0;
}
static void fp_mbtables(Footprint& f) {
    f.add((void*)(uintptr_t)CRT_MBCTYPE_VA, 0x101, "_mbctype");
    f.add((void*)(uintptr_t)CRT_MBCODEPAGE_VA, 8, "__mbcodepage, __mblcid");
    f.add((void*)(uintptr_t)CRT_MBULINFO_VA, 0x10, "__mbulinfo, fSystemSet");
}

// setSBCS (0x4d4250): no lead bytes, code page 0
static void __cdecl c_setSBCS() {
    mbctype_clear();
    G32(CRT_MBULINFO_VA) = 0;
    G32(CRT_MBCODEPAGE_VA) = 0;
    G32(CRT_MBLCID_VA) = 0;
    G32(CRT_MBULINFO_VA + 4) = 0;
    G32(CRT_MBULINFO_VA + 8) = 0;
}
PORT_FN(0x004d4250, "setSBCS", c_setSBCS, fp_mbtables)

// _setmbcp (0x4d3fc0): the code page's lead-byte ranges into _mbctype -- from the CRT's own table of four East Asian
// code pages, else from GetCPInfo (lead ranges _M1, and 1..0xfe _M2 for a double-byte page)
static int32_t __cdecl c_setmbcp(int32_t codepage) {
    const uint32_t cp = gcall<uint32_t>(F_getSystemCP, codepage);
    if (G32(CRT_MBCODEPAGE_VA) == cp) return 0;
    if (cp == 0) {
        gcall<void>(F_setSBCS);
        return 0;
    }
    uint32_t i = 0;
    for (uint32_t e = CRT_CPINFO_TAB_VA; e < CRT_CPINFO_TAB_END; e += 0x30, i++) {
        if (G32(e) != cp) continue;
        mbctype_clear();
        for (uint32_t j = 0; j < 4; j++) {
            uint32_t r = CRT_CPINFO_TAB_VA + 0x10 + i * 0x30 + j * 8;        // rgrange[i][j]
            if (G8(r) == 0) continue;
            while (G8(r + 1) != 0) {
                for (uint32_t c = G8(r); c <= G8(r + 1); c++) G8(CRT_MBCTYPE_VA + 1 + c) |= G8(CRT_RGCTYPEFLAG_VA + j);
                r += 2;
                if (G8(r) == 0) break;
            }
        }
        G32(CRT_MBCODEPAGE_VA) = cp;
        G32(CRT_MBLCID_VA) = gcall<uint32_t>(F_CPtoLCID, cp);
        const uint32_t m = CRT_CPINFO_TAB_VA + 4 + i * 0x30;              // mbulinfo, 3 dwords
        const uint32_t m1 = G32(m + 4);
        const uint32_t m0 = G32(m), m2 = G32(m + 8);
        G32(CRT_MBULINFO_VA) = m0;
        G32(CRT_MBULINFO_VA + 4) = m1;
        G32(CRT_MBULINFO_VA + 8) = m2;
        return 0;
    }
    CPINFO info;
    if (kGetCPInfo(cp, &info) != 1) {
        if (G32(CRT_FSYSTEMSET_VA)) {
            gcall<void>(F_setSBCS);
            return 0;
        }
        return -1;
    }
    mbctype_clear();
    uint32_t lcid;
    if (info.MaxCharSize > 1) {
        const uint8_t* lb = info.LeadByte;
        if (lb[0]) {
            while (lb[1]) {
                for (uint32_t c = lb[0]; c <= lb[1]; c++) G8(CRT_MBCTYPE_VA + 1 + c) |= 4;
                lb += 2;
                if (!lb[0]) break;
            }
        }
        for (uint32_t c = 1; c < 0xff; c++) G8(CRT_MBCTYPE_VA + 1 + c) |= 8;
        G32(CRT_MBCODEPAGE_VA) = cp;
        lcid = gcall<uint32_t>(F_CPtoLCID, cp);
    } else {
        lcid = 0;
        G32(CRT_MBCODEPAGE_VA) = 0;
    }
    G32(CRT_MBLCID_VA) = lcid;
    G32(CRT_MBULINFO_VA) = 0;
    G32(CRT_MBULINFO_VA + 4) = 0;
    G32(CRT_MBULINFO_VA + 8) = 0;
    return 0;
}
static void fp_setmbcp(Footprint& f, int32_t) { fp_mbtables(f); }
PORT_FN(0x004d3fc0, "setmbcp", c_setmbcp, fp_setmbcp)

// ___initmbctable (0x4d4280): _setmbcp(_MB_CP_ANSI)
static void __cdecl c_initmbctable() { gcall<int32_t>(F_setmbcp, -3); }
PORT_FN(0x004d4280, "initmbctable", c_initmbctable, fp_mbtables)

// _mbctoupper (0x4d48b0): a double-byte character through LCMapString (the code page's locale), else ASCII
static uint32_t __cdecl c_mbctoupper(uint32_t c) {
    if (c <= 0xff) return (int32_t)c >= 0x61 && (int32_t)c <= 0x7a ? c - 0x20 : c;
    alignas(4) uint8_t fr[8] = {};                  // esp+4 .. esp+0xb: the source at +2, the result at +4
    fr[2] = (uint8_t)(c >> 8);
    fr[3] = (uint8_t)c;
    if (!(G8(CRT_MBCTYPE_VA + 1 + fr[2]) & 4)) return c;
    if (!gcall<int32_t>(F_crtLCMapStringA, G32(CRT_MBLCID_VA), 0x200u, (const char*)&fr[2], 2, (char*)&fr[4], 2,
                        G32(CRT_MBCODEPAGE_VA)))
        return c;
    return ((uint32_t)fr[4] << 8) + fr[5];
}
static void fp_mbctoupper(Footprint& f, uint32_t) { f.add((void*)(uintptr_t)CRT_AWMAP_FUSE_VA, 4, "aw_map f_use"); }
PORT_FN(0x004d48b0, "mbctoupper", c_mbctoupper, fp_mbctoupper)

// mbtowc (0x4d6340)
static int32_t __cdecl c_mbtowc(uint16_t* pwc, const char* s, uint32_t n) {
    if (!s || !n) return 0;
    const uint8_t c = (uint8_t)*s;
    if (!c) {
        if (pwc) *pwc = 0;
        return 0;
    }
    if (crt_lc_ctype == 0) {
        if (pwc) *pwc = c;
        return 1;
    }
    if (crt_pctype[c] & 0x8000) {
        const int32_t mx = crt_mb_cur_max;
        bool ok;
        if (mx <= 1) ok = false;
        else if ((int32_t)n < mx) ok = false;
        else ok = kMultiByteToWideChar(crt_lc_codepage, 9, s, crt_mb_cur_max, (LPWSTR)pwc, pwc ? 1 : 0) != 0;
        if (!ok) {
            if (n < (uint32_t)crt_mb_cur_max || !s[1]) {
                crt_errno = CRT_EILSEQ;
                return -1;
            }
        }
        return crt_mb_cur_max;
    }
    if (!kMultiByteToWideChar(crt_lc_codepage, 9, s, 1, (LPWSTR)pwc, pwc ? 1 : 0)) {
        crt_errno = CRT_EILSEQ;
        return -1;
    }
    return 1;
}
static void fp_mbtowc(Footprint& f, uint16_t* pwc, const char*, uint32_t) {
    if (pwc) f.add(pwc, 2, "mbtowc out");
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno");
}
PORT_FN(0x004d6340, "mbtowc", c_mbtowc, fp_mbtowc)

// wctomb (0x4d66b0)
static int32_t __cdecl c_wctomb(char* s, uint32_t wc) {
    if (!s) return 0;
    if (crt_lc_ctype == 0) {
        if ((uint16_t)wc > 0xff) {
            crt_errno = CRT_EILSEQ;
            return -1;
        }
        *s = (char)wc;
        return 1;
    }
    BOOL defused = 0;
    const uint16_t w = (uint16_t)wc;
    const int32_t r = kWideCharToMultiByte(crt_lc_codepage, 0x220, (LPCWSTR)&w, 1, s, crt_mb_cur_max, 0, &defused);
    if (r == 0 || defused) {
        crt_errno = CRT_EILSEQ;
        return -1;
    }
    return r;
}
static void fp_wctomb(Footprint& f, char* s, uint32_t) {
    if (s) f.add(s, (uint32_t)(crt_mb_cur_max > 0 ? crt_mb_cur_max : 1), "wctomb out");
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno");
}
PORT_FN(0x004d66b0, "wctomb", c_wctomb, fp_wctomb)

// =====================================================================================================================
// aw_map.obj, aw_str.obj: the A or W API, whichever this Windows has (probed once)
// =====================================================================================================================
// strncnt (0x4d4d10): the length of s within cnt, or cnt (reading s[cnt] when it runs out)
static int32_t __cdecl c_strncnt(const char* s, int32_t cnt) {
    int32_t n = cnt;
    const char* p = s;
    while (n-- && *p) p++;
    if (!*p) return (int32_t)(p - s);
    return cnt;
}
static void fp_strncnt(Footprint& f, const char*, int32_t) { f.pure = true; }
PORT_FN(0x004d4d10, "strncnt", c_strncnt, fp_strncnt)

// __crtLCMapStringA (0x4d4ae0). Returns f_use itself when it holds neither 1 nor 2 (it can't: faithful)
static int32_t __cdecl c_crtLCMapStringA(uint32_t locale, uint32_t flags, const char* src, int32_t cch_src, char* dst,
                                         int32_t cch_dst, uint32_t code_page) {
    LCMapStringA_t lcmapA;
    int32_t use = G32(CRT_AWMAP_FUSE_VA);
    if (!use) {
        lcmapA = *(LCMapStringA_t volatile*)(uintptr_t)kLCMapStringA_slot;
        if (lcmapA(0, 0x100, (LPCSTR)(uintptr_t)CRT_EMPTY_A_VA, 1, 0, 0)) use = 2;
        else if (kLCMapStringW(0, 0x100, (LPCWSTR)(uintptr_t)CRT_EMPTY_W_VA, 1, 0, 0)) use = 1;
        else return 0;
    } else {
        lcmapA = *(LCMapStringA_t volatile*)(uintptr_t)kLCMapStringA_slot;
    }
    G32(CRT_AWMAP_FUSE_VA) = use;
    if (cch_src > 0) {
        cch_src = gcall<int32_t>(F_strncnt, src, cch_src);
        use = G32(CRT_AWMAP_FUSE_VA);
    }
    G32(CRT_AWMAP_FUSE_VA) = use;
    if (use == 2) return lcmapA(locale, flags, src, cch_src, dst, cch_dst);
    G32(CRT_AWMAP_FUSE_VA) = use;
    if (use != 1) return use;
    wchar_t* inw = 0;
    wchar_t* outw = 0;
    int32_t ret;
    if (!code_page) code_page = crt_lc_codepage;
    const int32_t insize = kMultiByteToWideChar(code_page, 9, src, cch_src, 0, 0);
    if (!insize) return 0;
    inw = gcall<wchar_t*>(F_malloc, (uint32_t)insize * 2);
    if (!inw) return 0;
    if (!kMultiByteToWideChar(code_page, 1, src, cch_src, inw, insize)) goto fail;
    ret = kLCMapStringW(locale, flags, inw, insize, 0, 0);
    if (!ret) goto fail;
    if (flags & 0x400) {                            // LCMAP_SORTKEY
        if (cch_dst) {
            if (ret > cch_dst) goto fail;
            if (!kLCMapStringW(locale, flags, inw, insize, (LPWSTR)dst, cch_dst)) goto fail;
        }
    } else {
        outw = gcall<wchar_t*>(F_malloc, (uint32_t)ret * 2);
        if (!outw) goto fail;
        if (!kLCMapStringW(locale, flags, inw, insize, outw, ret)) goto fail;
        if (!cch_dst) ret = kWideCharToMultiByte(code_page, 0x220, outw, ret, 0, 0, 0, 0);
        else ret = kWideCharToMultiByte(code_page, 0x220, outw, ret, dst, cch_dst, 0, 0);
        if (!ret) goto fail;
    }
    gcall<void>(F_free, inw);
    gcall<void>(F_free, outw);
    return ret;
fail:
    gcall<void>(F_free, inw);
    gcall<void>(F_free, outw);
    return 0;
}
static void fp_crtLCMapStringA(Footprint& f, uint32_t, uint32_t, const char*, int32_t, char*, int32_t, uint32_t) {
    f.replay_only = "allocates on the wide-character path (a non-C locale or a double-byte code page only)";
}
PORT_FN(0x004d4ae0, "crtLCMapStringA", c_crtLCMapStringA, fp_crtLCMapStringA)

// __crtGetStringTypeA (0x4d68f0)
static int32_t __cdecl c_crtGetStringTypeA(uint32_t info_type, const char* src, int32_t cch_src, uint16_t* types,
                                           uint32_t code_page, uint32_t lcid) {
    GetStringTypeA_t gstA;
    int32_t use = G32(CRT_AWSTR_FUSE_VA);
    if (!use) {
        alignas(4) uint8_t fr[4];                   // the probe's dummy word at +2 of the frame's one local
        gstA = *(GetStringTypeA_t volatile*)(uintptr_t)kGetStringTypeA_slot;
        if (gstA(0, 1, (LPCSTR)(uintptr_t)CRT_EMPTY_A_VA, 1, (LPWORD)&fr[2])) use = 2;
        else if (kGetStringTypeW(1, (LPCWSTR)(uintptr_t)CRT_EMPTY_W_VA, 1, (LPWORD)&fr[2])) use = 1;
        else return 0;
    } else {
        gstA = *(GetStringTypeA_t volatile*)(uintptr_t)kGetStringTypeA_slot;
    }
    G32(CRT_AWSTR_FUSE_VA) = use;
    if (use == 2) {
        if (!lcid) lcid = crt_lc_ctype;
        return gstA(lcid, info_type, src, cch_src, (LPWORD)types);
    }
    G32(CRT_AWSTR_FUSE_VA) = use;
    if (use != 1) return use;
    int32_t ret = 0;
    wchar_t* w = 0;
    if (!code_page) code_page = crt_lc_codepage;
    const int32_t size = kMultiByteToWideChar(code_page, 9, src, cch_src, 0, 0);
    if (size) {
        w = gcall<wchar_t*>(F_calloc, 2u, (uint32_t)size);
        if (w) {
            const int32_t r1 = kMultiByteToWideChar(code_page, 1, src, cch_src, w, size);
            if (r1) ret = kGetStringTypeW(info_type, w, r1, (LPWORD)types);
        }
    }
    gcall<void>(F_free, w);
    return ret;
}
static void fp_crtGetStringTypeA(Footprint& f, uint32_t, const char*, int32_t, uint16_t*, uint32_t, uint32_t) {
    f.replay_only = "allocates on the wide-character path (a Windows without GetStringTypeA only)";
}
PORT_FN(0x004d68f0, "crtGetStringTypeA", c_crtGetStringTypeA, fp_crtGetStringTypeA)

}   // namespace crt_str
