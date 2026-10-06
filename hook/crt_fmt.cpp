// crt_fmt.cpp -- M3 stage LIBC, group A: the C runtime's printf and scanf engines and their front ends, rewritten:
// output.obj (_output, write_char / write_multi_char / write_string, get_int_arg / get_int64_arg / get_short_arg),
// input.obj (_input, _hextodec / _inc / _un_inc / _whiteout), sprintf, vsprintf, sscanf, printf, fprintf.
//
// Written from the v1.0 disassembly (Microsoft's ~1997 LIBC.lib, output.c / input.c), faithful: the same state
// machine (output reads its class / transition table, __lookuptable, from the CRT's .rdata), the same calls in the same
// order to the same v1.0 addresses (write_char per character, get_int_arg per argument, _aullrem / _aulldiv per digit,
// _cfltcvt / _cropzeros / _forcdecpt / _fassign through _cfltcvt_tab), and every quirk:
//   * printf: a precision is never clamped to the 512-byte buffer (%.600f overruns it -- FIX CANDIDATE, CRT-internal);
//     -0.0 prints without its sign; an unknown conversion character prints what the previous conversion left in the
//     buffer (its text and length; for the first conversion of a call the original's are stack garbage, here empty);
//     a lead byte at the end of the format writes the terminator and reads past it; %S / %ls counts a wctomb failure
//     (-1) into the length.
//   * scanf: a range ending in \xff ("%[a-\xff]") never ends (an 8-bit loop counter -- FIX CANDIDATE); trailing white
//     space in the format reads one more character; %I64p reads decimal; %I64n stores the (cleared) 64-bit number, not
//     the count; the returned EOF test reads the last character read, which for a format that reads nothing is the
//     original's stack garbage (here 0: no EOF).
// Checked offline by test/world_crt_fmt.cpp against the original on random formats and arguments.
//
// The variadic front ends take a `Va` window in place of `...` (as krn_file.cpp): 32 dwords over the caller's pushed
// arguments, whose address is the va_list. A shadow check forwards the same window to both passes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "crt_types.h"

#if defined(VP_FUZZ) || defined(VP_CRT_HARNESS)
#define CRT_SHADOW_ORIGINAL_FMT(NEW)
#else
// a register-convention routine can't go through the C checking wrapper: in shadow mode it stays original
#ifdef VP_GCC
#define CRT_SHADOW_ORIGINAL_FMT(NEW)                                                                               \
    static __declspec(naked) void VP_CAT(shadow_orig_, NEW)() {                                                    \
        __asm__ volatile("jmp dword ptr [%c0]" : : "i"(&VP_CAT(port_, NEW).orig));                                \
    }                                                                                                              \
    namespace { const bool VP_CAT(shadow_orig_set_, NEW) = (VP_CAT(port_, NEW).shadow = (void*)&VP_CAT(shadow_orig_, NEW), true); }
#else
#define CRT_SHADOW_ORIGINAL_FMT(NEW)                                                                               \
    static __declspec(naked) void VP_CAT(shadow_orig_, NEW)() {                                                    \
        __asm { jmp dword ptr [VP_CAT(port_, NEW).orig] }                                                           \
    }                                                                                                              \
    namespace { const bool VP_CAT(shadow_orig_set_, NEW) = (VP_CAT(port_, NEW).shadow = (void*)&VP_CAT(shadow_orig_, NEW), true); }
#endif
#endif

namespace {

typedef uint32_t U32;
#define CRT_CALL(T, va) ((T)(uintptr_t)(va))

struct Va {                                    // the variadic window (see the top)
    uint32_t w[32];
    Va() = default;
    explicit Va(uint32_t) {}                    // (test/fuzz.h builds arguments with a cast)
};

enum : U32 {
    A_OUTPUT = 0x004d22b0, A_WRITE_CHAR = 0x004d2c90, A_WRITE_MULTI_CHAR = 0x004d2ce0, A_WRITE_STRING = 0x004d2d20,
    A_GET_INT_ARG = 0x004d2d60, A_GET_INT64_ARG = 0x004d2d70, A_GET_SHORT_ARG = 0x004d2d90,
    A_INPUT = 0x004d0ee0, A_HEXTODEC = 0x004d1b80, A_INC = 0x004d1bc0, A_UN_INC = 0x004d1bf0, A_WHITEOUT = 0x004d1c10,
    A_FLSBUF = 0x004d2160, A_FILBUF = 0x004d52c0, A_UNGETC = 0x004d6450, A_STBUF = 0x004d4ee0, A_FTBUF = 0x004d4f80,
    AF_ISCTYPE = 0x004d47a0, A_ISSPACE = 0x004cf790, A_WCTOMB = 0x004d66b0, A_MBTOWC = 0x004d6340,
    A_AULLREM = 0x004d67c0, A_AULLDIV = 0x004d6750, A_ALLMUL = 0x004d6300,
    G_LOOKUPTABLE = 0x004e0200,                 // __lookuptable (.rdata): class in the low nibble, then the transitions
    G_NULLSTRING = 0x00502848,                  // __nullstring -> "(null)"
    G_WNULLSTRING = 0x0050284c,                 // __wnullstring -> L"(null)"
    G_SBRACKSET = 0x00502724,                   // " \t-\r]": %s's white space (a range: \t..\r)
    G_CBRACKSET = 0x0050272c,                   // "]": %c's empty set
};
#define c_output            CRT_CALL(int(__cdecl*)(CrtFile*, const char*, char*), A_OUTPUT)
#define c_write_char        CRT_CALL(void(__cdecl*)(int, CrtFile*, int*), A_WRITE_CHAR)
#define c_write_multi_char  CRT_CALL(void(__cdecl*)(int, int, CrtFile*, int*), A_WRITE_MULTI_CHAR)
#define c_write_string      CRT_CALL(void(__cdecl*)(const char*, int, CrtFile*, int*), A_WRITE_STRING)
#define c_get_int_arg       CRT_CALL(int(__cdecl*)(char**), A_GET_INT_ARG)
#define c_get_int64_arg     CRT_CALL(uint64_t(__cdecl*)(char**), A_GET_INT64_ARG)
#define c_get_short_arg     CRT_CALL(uint16_t(__cdecl*)(char**), A_GET_SHORT_ARG)
#define c_input             CRT_CALL(int(__cdecl*)(CrtFile*, const uint8_t*, char*), A_INPUT)
#define c_hextodec          CRT_CALL(int(__cdecl*)(int), A_HEXTODEC)
#define c_inc               CRT_CALL(int(__cdecl*)(CrtFile*), A_INC)
#define c_un_inc            CRT_CALL(void(__cdecl*)(int, CrtFile*), A_UN_INC)
#define c_whiteout          CRT_CALL(int(__cdecl*)(int*, CrtFile*), A_WHITEOUT)
#define c_flsbuf            CRT_CALL(int(__cdecl*)(int, CrtFile*), A_FLSBUF)
#define c_filbuf            CRT_CALL(int(__cdecl*)(CrtFile*), A_FILBUF)
#define c_ungetc            CRT_CALL(int(__cdecl*)(int, CrtFile*), A_UNGETC)
#define c_stbuf             CRT_CALL(int(__cdecl*)(CrtFile*), A_STBUF)
#define c_ftbuf             CRT_CALL(void(__cdecl*)(int, CrtFile*), A_FTBUF)
#define cf_isctype           CRT_CALL(int(__cdecl*)(int, int), AF_ISCTYPE)
#define c_isspace           CRT_CALL(int(__cdecl*)(int), A_ISSPACE)
#define c_wctomb            CRT_CALL(int(__cdecl*)(char*, U32), A_WCTOMB)
#define c_mbtowc            CRT_CALL(int(__cdecl*)(uint16_t*, const char*, U32), A_MBTOWC)
#define c_aullrem           CRT_CALL(uint64_t(__stdcall*)(uint64_t, uint64_t), A_AULLREM)
#define c_aulldiv           CRT_CALL(uint64_t(__stdcall*)(uint64_t, uint64_t), A_AULLDIV)
#define c_allmul            CRT_CALL(uint64_t(__stdcall*)(uint64_t, uint64_t), A_ALLMUL)
// _cfltcvt_tab's routines, read from the table at each call (until _cfltcvt_init they are _fptrap)
#define p_cfltcvt   (*(char*(__cdecl* volatile*)(void*, char*, int, int, int))(uintptr_t)CRT_CFLTCVT_VA)
#define p_cropzeros (*(void(__cdecl* volatile*)(char*))(uintptr_t)CRT_CROPZEROS_VA)
#define p_fassign   (*(void(__cdecl* volatile*)(int, char*, char*))(uintptr_t)CRT_FASSIGN_VA)
#define p_forcdecpt (*(void(__cdecl* volatile*)(char*))(uintptr_t)CRT_FORCDECPT_VA)

static inline int isctype_inl(int c, int mask) {      // the compiler's inline isdigit / isxdigit / isspace
    if (crt_mb_cur_max > 1) return cf_isctype(c, mask);
    return crt_pctype[c] & mask;
}

// output's flags
enum {
    FL_SIGN = 0x1, FL_SIGNSP = 0x2, FL_LEFT = 0x4, FL_LEADZERO = 0x8, FL_LONG = 0x10, FL_SHORT = 0x20, FL_SIGNED = 0x40,
    FL_ALTERNATE = 0x80, FL_NEGATIVE = 0x100, FL_FORCEOCTAL = 0x200, FL_WIDECHAR = 0x800, FL_I64 = 0x8000,
};

// What a shadow check's footprint needs ahead of the call: a dry run of the same engine (output: count the characters;
// input: record where each store would go). Not a pass of its own: the real call is made by the check.
struct OutDry { int count; };
struct InDry {
    Footprint* fp;
    uint8_t* last;
    U32 last_n;
    void store(void* p, U32 n) {                // merge a run of adjacent stores (a string) into one region
        if (last && (uint8_t*)p == last + last_n) { last_n += n; return; }
        flush();
        last = (uint8_t*)p;
        last_n = n;
    }
    void flush() {
        if (last && fp) fp->add(last, last_n, "scanf destination");
        last = 0;
        last_n = 0;
    }
};

}  // namespace

// =====================================================================================================================
// output.obj
// =====================================================================================================================
// write_char (0x4d2c90): into the stream's buffer (read back from it) or through _flsbuf; -1 marks a failure
static void __cdecl crt_write_char(int ch, CrtFile* f, int* pnumwritten) {
    int r;
    if (--f->cnt >= 0) {
        *f->ptr = (char)ch;
        r = (uint8_t)*f->ptr++;
    } else {
        r = c_flsbuf(ch, f);
    }
    if (r == -1) *pnumwritten = -1;
    else (*pnumwritten)++;
}
static void fp_write_char(Footprint& f, int, CrtFile* s, int*) { f.replay_only = "writes a stream (_flsbuf: file I/O)"; (void)s; }
PORT_FN(0x004d2c90, "write_char", crt_write_char, fp_write_char)

// write_multi_char (0x4d2ce0): ch, num times (stops at a failure)
static void __cdecl crt_write_multi_char(int ch, int num, CrtFile* f, int* pnumwritten) {
    while (num-- > 0) {
        c_write_char(ch, f, pnumwritten);
        if (*pnumwritten == -1) break;
    }
}
static void fp_write_multi_char(Footprint& f, int, int, CrtFile*, int*) { f.replay_only = "writes a stream (_flsbuf: file I/O)"; }
PORT_FN(0x004d2ce0, "write_multi_char", crt_write_multi_char, fp_write_multi_char)

// write_string (0x4d2d20): len characters (sign-extended into write_char)
static void __cdecl crt_write_string(const char* string, int len, CrtFile* f, int* pnumwritten) {
    while (len-- > 0) {
        c_write_char((int)(signed char)*string++, f, pnumwritten);
        if (*pnumwritten == -1) break;
    }
}
static void fp_write_string(Footprint& f, const char*, int, CrtFile*, int*) { f.replay_only = "writes a stream (_flsbuf: file I/O)"; }
PORT_FN(0x004d2d20, "write_string", crt_write_string, fp_write_string)

// get_int_arg (0x4d2d60) / get_int64_arg (0x4d2d70) / get_short_arg (0x4d2d90): the next argument. In assembly:
// get_short_arg returns only ax (eax's top half is the va_list pointer's), get_int64_arg leaves ecx = the old pointer.
static __declspec(naked) int __cdecl crt_get_int_arg(char**) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 4]\n\t"
        "mov ecx, dword ptr [eax]\n\t"
        "add ecx, 4\n\t"
        "mov dword ptr [eax], ecx\n\t"
        "mov eax, dword ptr [ecx - 4]\n\t"
        "ret"
        : :);
#else
    __asm {
        mov eax, dword ptr [esp + 4]
        mov ecx, dword ptr [eax]
        add ecx, 4
        mov dword ptr [eax], ecx
        mov eax, dword ptr [ecx - 4]
        ret
    }
#endif
}
static void fp_get_arg(Footprint& f, char** pargptr) { f.add(pargptr, 4, "va_list"); }
PORT_FN(0x004d2d60, "get_int_arg", crt_get_int_arg, fp_get_arg)

static __declspec(naked) uint64_t __cdecl crt_get_int64_arg(char**) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 4]\n\t"
        "mov ecx, dword ptr [eax]\n\t"
        "add ecx, 8\n\t"
        "mov dword ptr [eax], ecx\n\t"
        "mov eax, dword ptr [ecx - 8]\n\t"
        "mov edx, dword ptr [ecx - 4]\n\t"
        "sub ecx, 8\n\t"
        "ret"
        : :);
#else
    __asm {
        mov eax, dword ptr [esp + 4]
        mov ecx, dword ptr [eax]
        add ecx, 8
        mov dword ptr [eax], ecx
        mov eax, dword ptr [ecx - 8]
        mov edx, dword ptr [ecx - 4]
        sub ecx, 8
        ret
    }
#endif
}
PORT_FN(0x004d2d70, "get_int64_arg", crt_get_int64_arg, fp_get_arg)

static __declspec(naked) uint16_t __cdecl crt_get_short_arg(char**) {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [esp + 4]\n\t"
        "mov ecx, dword ptr [eax]\n\t"
        "add ecx, 4\n\t"
        "mov dword ptr [eax], ecx\n\t"
        "mov ax, word ptr [ecx - 4]\n\t"
        "ret"
        : :);
#else
    __asm {
        mov eax, dword ptr [esp + 4]
        mov ecx, dword ptr [eax]
        add ecx, 4
        mov dword ptr [eax], ecx
        mov ax, word ptr [ecx - 4]
        ret
    }
#endif
}
PORT_FN(0x004d2d90, "get_short_arg", crt_get_short_arg, fp_get_arg)

// _output (0x4d22b0): the printf engine. `dry`: count only (a footprint's look ahead; %n stores nothing).
static int output_engine(CrtFile* stream, const char* format, char* argptr, OutDry* dry) {
    const uint8_t* const lookup = (const uint8_t*)(uintptr_t)G_LOOKUPTABLE;
    int charsout = 0;
    int state = 0;
    // these live across conversions as the original's registers and frame slots do (see the top: an unknown type)
    U32 flags = 0;
    int textlen = 0;
    char buffer[512];
    char* text = buffer;
    char prefix[2] = {0, 0};
    int hexadd = 0, fldwidth = 0, precision = 0, prefixlen = 0, bufferiswide = 0, no_output = 0, capexp = 0;
    int padding = 0;
    char mbuf[8];                               // wctomb's buffer for %S output (frame +0x14)
    char lenbuf[8];                             // ... and for its length count (frame +0x10)

#define WCHAR(c) (dry ? (void)(dry->count++) : c_write_char((c), stream, &charsout))
#define WMULTI(c, n) (dry ? (void)(dry->count += ((n) > 0 ? (n) : 0)) : c_write_multi_char((c), (n), stream, &charsout))
#define WSTRING(s, n) (dry ? (void)(dry->count += ((n) > 0 ? (n) : 0)) : c_write_string((s), (n), stream, &charsout))

    char ch = *format++;
    while (ch != 0 && charsout >= 0) {
        const int chclass = (ch >= ' ' && ch <= 'x') ? (lookup[ch - 0x20] & 0xf) : 0;
        state = (int)(signed char)lookup[chclass * 8 + state] >> 4;
        switch (state) {
        case 0:                                                       // ST_NORMAL
        normal_state:
            bufferiswide = 0;
            if (crt_pctype[(uint8_t)ch] & CRT_LEADBYTE) {
                WCHAR((int)ch);
                ch = *format++;
            }
            WCHAR((int)ch);
            break;
        case 1:                                                       // ST_PERCENT
            no_output = capexp = fldwidth = prefixlen = 0;
            precision = -1;
            flags = 0;
            bufferiswide = 0;
            break;
        case 2:                                                       // ST_FLAG
            switch (ch) {
            case ' ': flags |= FL_SIGNSP; break;
            case '#': flags |= FL_ALTERNATE; break;
            case '+': flags |= FL_SIGN; break;
            case '-': flags |= FL_LEFT; break;
            case '0': flags |= FL_LEADZERO; break;
            }
            break;
        case 3:                                                       // ST_WIDTH
            if (ch == '*') {
                fldwidth = c_get_int_arg(&argptr);
                if (fldwidth < 0) {
                    flags |= FL_LEFT;
                    fldwidth = -fldwidth;
                }
            } else {
                fldwidth = fldwidth * 10 + ((int)ch - '0');
            }
            break;
        case 4:                                                       // ST_DOT
            precision = 0;
            break;
        case 5:                                                       // ST_PRECIS
            if (ch == '*') {
                precision = c_get_int_arg(&argptr);
                if (precision < 0) precision = -1;
            } else {
                precision = precision * 10 + ((int)ch - '0');
            }
            break;
        case 6:                                                       // ST_SIZE
            switch (ch) {
            case 'I':
                if (format[0] == '6' && format[1] == '4') {
                    format += 2;
                    flags |= FL_I64;
                } else {
                    state = 0;
                    goto normal_state;
                }
                break;
            case 'h': flags |= FL_SHORT; break;
            case 'l': flags |= FL_LONG; break;
            case 'w': flags |= FL_WIDECHAR; break;
            }
            break;
        case 7: {                                                     // ST_TYPE
            int radix = 0;
            switch (ch) {
            case 'C':
                if (!(flags & (FL_SHORT | FL_LONG | FL_WIDECHAR))) flags |= FL_WIDECHAR;
                // fall through
            case 'c':
                if (flags & (FL_LONG | FL_WIDECHAR)) {
                    const uint16_t wch = c_get_short_arg(&argptr);
                    textlen = c_wctomb(buffer, wch);
                    if (textlen < 0) no_output = 1;
                } else {
                    textlen = 1;
                    buffer[0] = (char)c_get_int_arg(&argptr);
                }
                text = buffer;
                goto common;
            case 'E':
            case 'G':
                capexp = 1;
                ch = (char)(ch + 0x20);
                // fall through
            case 'e':
            case 'f':
            case 'g': {
                flags |= FL_SIGNED;
                text = buffer;
                if (precision < 0) precision = 6;
                else if (precision == 0 && ch == 'g') precision = 1;
                argptr += 8;
                uint32_t tmp[2];                                       // the double, moved as two dwords
                memcpy(tmp, argptr - 8, 8);
                p_cfltcvt(tmp, buffer, (int)ch, precision, capexp);
                const U32 alt = flags & FL_ALTERNATE;
                if (alt && precision == 0) p_forcdecpt(buffer);
                if (ch == 'g' && !alt) p_cropzeros(buffer);
                if (buffer[0] == '-') {
                    flags |= FL_NEGATIVE;
                    text = buffer + 1;
                }
                textlen = (int)strlen(text);
                goto common;
            }
            case 'S':
                if (!(flags & (FL_SHORT | FL_LONG | FL_WIDECHAR))) flags |= FL_WIDECHAR;
                // fall through
            case 's': {
                const int i = precision == -1 ? 0x7fffffff : precision;
                text = (char*)(uintptr_t)(U32)c_get_int_arg(&argptr);
                if (flags & (FL_LONG | FL_WIDECHAR)) {
                    if (!text) text = *(char* volatile*)(uintptr_t)G_WNULLSTRING;
                    const uint16_t* p = (const uint16_t*)text;
                    textlen = 0;
                    bufferiswide = 1;
                    if (i > 0) {
                        while (*p) {
                            const int len = c_wctomb(lenbuf, *p);
                            if (len == 0) break;
                            textlen += len;
                            p++;
                            if (textlen >= i) break;
                        }
                    }
                } else {
                    if (!text) text = *(char* volatile*)(uintptr_t)G_NULLSTRING;
                    const char* p = text;
                    int n = i;
                    while (n-- && *p) p++;
                    textlen = (int)(p - text);
                }
                goto common;
            }
            case 'Z': {
                struct CountedString { int16_t Length, MaximumLength; char* Buffer; };
                const CountedString* pstr = (const CountedString*)(uintptr_t)(U32)c_get_int_arg(&argptr);
                if (pstr == 0 || pstr->Buffer == 0) {
                    text = *(char* volatile*)(uintptr_t)G_NULLSTRING;
                    textlen = (int)strlen(text);
                } else if (flags & FL_WIDECHAR) {
                    bufferiswide = 1;
                    textlen = (int)((U32)(int32_t)pstr->Length >> 1);
                    text = pstr->Buffer;
                } else {
                    bufferiswide = 0;
                    textlen = (int)pstr->Length;
                    text = pstr->Buffer;
                }
                goto common;
            }
            case 'n': {
                void* p = (void*)(uintptr_t)(U32)c_get_int_arg(&argptr);
                if (!dry) {
                    if (flags & FL_SHORT) *(int16_t*)p = (int16_t)charsout;
                    else *(int32_t*)p = charsout;
                }
                no_output = 1;
                goto common;
            }
            case 'd':
            case 'i':
                flags |= FL_SIGNED;
                radix = 10;
                goto common_int;
            case 'u':
                radix = 10;
                goto common_int;
            case 'o':
                radix = 8;
                if (flags & FL_ALTERNATE) flags |= FL_FORCEOCTAL;
                goto common_int;
            case 'p':
                precision = 8;
                // fall through
            case 'X':
                hexadd = 7;
                goto common_hex;
            case 'x':
                hexadd = 0x27;
            common_hex:
                radix = 16;
                if (flags & FL_ALTERNATE) {
                    prefix[0] = '0';
                    prefixlen = 2;
                    prefix[1] = (char)(hexadd + 0x51);
                }
            common_int: {
                uint64_t number;
                if (flags & FL_I64) {
                    number = c_get_int64_arg(&argptr);
                } else if (flags & FL_SHORT) {
                    if (flags & FL_SIGNED) number = (uint64_t)(int64_t)(int16_t)c_get_int_arg(&argptr);
                    else number = (uint64_t)(uint16_t)c_get_int_arg(&argptr);
                } else if (flags & FL_SIGNED) {
                    number = (uint64_t)(int64_t)(int32_t)c_get_int_arg(&argptr);
                } else {
                    number = (uint64_t)(U32)c_get_int_arg(&argptr);
                }
                uint64_t numbr;
                if ((flags & FL_SIGNED) && (int32_t)(U32)(number >> 32) < 0) {
                    numbr = 0 - number;
                    flags |= FL_NEGATIVE;
                } else {
                    numbr = number;
                }
                if (!(flags & FL_I64)) numbr &= 0xffffffffu;
                if (precision < 0) precision = 1;
                else flags &= ~(U32)FL_LEADZERO;
                if (numbr == 0) prefixlen = 0;
                text = &buffer[511];
                const uint64_t r64 = (uint64_t)(int64_t)radix;
                for (;;) {
                    const int p0 = precision--;
                    if (!(p0 > 0 || numbr != 0)) break;
                    int digit = (int)(U32)c_aullrem(numbr, r64) + '0';
                    numbr = c_aulldiv(numbr, r64);
                    if (digit > '9') digit += hexadd;
                    *text-- = (char)digit;
                }
                textlen = (int)(&buffer[511] - text);
                text++;
                if ((flags & FL_FORCEOCTAL) && (*text != '0' || textlen == 0)) {
                    textlen++;
                    *--text = '0';
                }
                goto common;
            }
            default:
                goto common;
            }
        common:
            if (no_output) break;
            if (flags & FL_SIGNED) {
                if (flags & FL_NEGATIVE) prefix[0] = '-';
                else if (flags & FL_SIGN) prefix[0] = '+';
                else if (flags & FL_SIGNSP) prefix[0] = ' ';
                else goto no_prefix;
                prefixlen = 1;
            }
        no_prefix:
            padding = fldwidth - textlen - prefixlen;
            if (!(flags & (FL_LEFT | FL_LEADZERO))) WMULTI(' ', padding);
            WSTRING(prefix, prefixlen);
            if ((flags & FL_LEADZERO) && !(flags & FL_LEFT)) WMULTI('0', padding);
            if (bufferiswide && textlen > 0) {
                const uint16_t* p = (const uint16_t*)text;
                int i = textlen;
                while (i--) {
                    const int retval = c_wctomb(mbuf, *p++);
                    if (retval <= 0) break;
                    WSTRING(mbuf, retval);
                }
            } else {
                WSTRING(text, textlen);
            }
            if (flags & FL_LEFT) WMULTI(' ', padding);
            break;
        }
        default:
            break;
        }
        ch = *format++;
    }
#undef WCHAR
#undef WMULTI
#undef WSTRING
    return dry ? dry->count : charsout;
}

static int __cdecl crt_output(CrtFile* stream, const char* format, char* argptr) {
    return output_engine(stream, format, argptr, 0);
}
static void fp_output(Footprint& f, CrtFile*, const char*, char*) { f.replay_only = "writes a stream (_flsbuf: file I/O)"; }
PORT_FN(0x004d22b0, "output", crt_output, fp_output)

// =====================================================================================================================
// input.obj
// =====================================================================================================================
// _hextodec (0x4d1b80): a hex digit's value + '0' (so the caller's - '0' gives it)
static int __cdecl crt_hextodec(int chr) {
    if (isctype_inl(chr, CRT_DIGIT)) return chr;
    return (int)((U32)chr & 0xffffffdfu) - 7;
}
static void fp_hextodec(Footprint&, int) {}
PORT_FN(0x004d1b80, "hextodec", crt_hextodec, fp_hextodec)

// _inc (0x4d1bc0): the next character, or _filbuf's
static int __cdecl crt_inc(CrtFile* f) {
    if (--f->cnt >= 0) return (uint8_t)*f->ptr++;
    return c_filbuf(f);
}
static void fp_inc(Footprint& f, CrtFile*) { f.replay_only = "reads a stream (_filbuf: file I/O)"; }
PORT_FN(0x004d1bc0, "inc", crt_inc, fp_inc)

// _un_inc (0x4d1bf0): ungetc unless EOF
static void __cdecl crt_un_inc(int chr, CrtFile* f) {
    if (chr != -1) c_ungetc(chr, f);
}
static void fp_un_inc(Footprint& f, int, CrtFile*) { f.replay_only = "a stream (ungetc may allocate a buffer)"; }
PORT_FN(0x004d1bf0, "un_inc", crt_un_inc, fp_un_inc)

// _whiteout (0x4d1c10): past white space, counting each character read; the first other one
static int __cdecl crt_whiteout(int* counter, CrtFile* f) {
    int ch;
    do {
        (*counter)++;
        ch = c_inc(f);
    } while (c_isspace(ch));
    return ch;
}
static void fp_whiteout(Footprint& f, int*, CrtFile*) { f.replay_only = "reads a stream (_filbuf: file I/O)"; }
PORT_FN(0x004d1c10, "whiteout", crt_whiteout, fp_whiteout)

// _input (0x4d0ee0): the scanf engine. `dry`: record each store's destination instead of making it (a footprint's
// look ahead).
static int input_engine(CrtFile* stream, const uint8_t* format, char* arglist, InDry* dry) {
    int charcount = 0, count = 0;
    int8_t match = 0;
    int ch = 0;                                                       // (the original's is stack garbage until read)
    char* pointer = 0;                                                // (likewise until the first assignment)
    char* arglistsave = 0;
    char floatstring[0x15e + 2];
    uint8_t table[32];
    uint64_t num64 = 0;

    while (*format) {
        if (isctype_inl(*format, CRT_SPACE)) {
            charcount--;
            c_un_inc(c_whiteout(&charcount, stream), stream);
            while (c_isspace(*++format)) {}
        }
        if (*format == '%') {
            U32 number = 0;
            int width = 0, started = 0, widthset = 0;
            uint8_t prevchar = 0, reject = 0;
            int negative = 0, suppress = 0, done_flag = 0, fl_wchar_arg = 0, integer64 = 0;
            int8_t widechar = 0, longone = 1;
            int comchr;
            while (!done_flag) {
                comchr = *++format;
                if (isctype_inl((uint8_t)comchr, CRT_DIGIT)) {
                    widthset++;
                    width = width * 10 + comchr - '0';
                } else {
                    switch (comchr) {
                    case '*': suppress++; break;
                    case 'F': break;
                    case 'I':
                        if (format[1] == '6' && format[2] == '4') {
                            format += 2;
                            integer64++;
                            num64 = 0;
                        } else {
                            done_flag++;
                        }
                        break;
                    case 'L': longone++; break;
                    case 'N': break;
                    case 'h': longone--; widechar--; break;
                    case 'l': longone++; widechar++; break;
                    case 'w': widechar++; break;
                    default: done_flag++; break;
                    }
                }
            }
            if (!suppress) {
                arglistsave = arglist;
                arglist += 4;
                pointer = *(char**)(arglist - 4);
            }
            done_flag = 0;
            if (!widechar) {
                if (*format == 'S' || *format == 'C') widechar++;
                else widechar--;
            }
            comchr = (uint8_t)(*format | 0x20);
            if (comchr != 'n') {
                if (comchr != 'c' && comchr != '{') {
                    ch = c_whiteout(&charcount, stream);
                } else {
                    charcount++;
                    ch = c_inc(stream);
                }
            }
            if (widthset && !width) goto error_return;
            const uint8_t* scanptr;
            char* start;
            switch (comchr) {
            case 'c':
                if (!widthset) {
                    width++;
                    widthset++;
                }
                if (widechar > 0) fl_wchar_arg = 1;
                scanptr = (const uint8_t*)(uintptr_t)G_CBRACKSET;
                reject = 0xff;
                goto scanit2;
            case 's':
                if (widechar > 0) fl_wchar_arg = 1;
                scanptr = (const uint8_t*)(uintptr_t)G_SBRACKSET;
                reject = 0xff;
                goto scanit2;
            case '{':
                if (widechar > 0) fl_wchar_arg = 1;
                scanptr = ++format;
                if (*scanptr == '^') {
                    scanptr++;
                    reject = 0xff;
                }
            scanit2:
                memset(table, 0, sizeof table);
                if (comchr == '{' && *scanptr == ']') {
                    prevchar = ']';
                    table[']' >> 3] = 1 << (']' & 7);
                    scanptr++;
                }
                while (*scanptr != ']') {
                    uint8_t rngch = *scanptr++;
                    if (rngch == '-' && prevchar && *scanptr != ']') {
                        rngch = *scanptr++;
                        uint8_t last;
                        if (prevchar < rngch) {
                            last = rngch;
                        } else {
                            last = prevchar;
                            prevchar = rngch;
                        }
                        // an 8-bit counter: a range ending in 0xff never ends (as the original's)
                        for (uint8_t r = prevchar; last >= r; r++) table[r >> 3] |= (uint8_t)(1 << (r & 7));
                        prevchar = 0;
                    } else {
                        prevchar = rngch;
                        table[rngch >> 3] |= (uint8_t)(1 << (rngch & 7));
                    }
                }
                if (!*scanptr) goto error_return_no_unget;
                if (comchr == '{') format = scanptr;
                start = pointer;
                charcount--;
                c_un_inc(ch, stream);
                for (;;) {
                    if (widthset) {
                        const int w0 = width--;
                        if (!w0) goto scan_done;
                    }
                    charcount++;
                    ch = c_inc(stream);
                    if (ch == -1) break;
                    if (!(((int)(signed char)(table[ch >> 3] ^ reject)) & (1 << (ch & 7)))) break;
                    if (!suppress) {
                        if (fl_wchar_arg) {
                            char temp[2];
                            temp[0] = (char)ch;
                            if (crt_pctype[(uint8_t)ch] & CRT_LEADBYTE) {
                                charcount++;
                                temp[1] = (char)c_inc(stream);
                            }
                            uint16_t wctemp;
                            c_mbtowc(&wctemp, temp, (U32)crt_mb_cur_max);
                            if (dry) dry->store(pointer, 2);
                            else memcpy(pointer, &wctemp, 2);
                            pointer += 2;
                        } else {
                            if (dry) dry->store(pointer, 1);
                            else *pointer = (char)ch;
                            pointer++;
                        }
                    } else {
                        start++;
                    }
                }
                charcount--;
                c_un_inc(ch, stream);
            scan_done:
                if (start == pointer) goto error_return_no_unget;
                if (!suppress) {
                    count++;
                    if (comchr != 'c') {
                        if (fl_wchar_arg) {
                            if (dry) dry->store(pointer, 2);
                            else *(uint16_t*)pointer = 0;
                        } else {
                            if (dry) dry->store(pointer, 1);
                            else *pointer = 0;
                        }
                    }
                }
                break;
            case 'e':
            case 'f':
            case 'g': {
                char* p = floatstring;
                if (ch == '-') {
                    *p++ = '-';
                    goto f_incwidth;
                } else if (ch == '+') {
                f_incwidth:
                    width--;
                    charcount++;
                    ch = c_inc(stream);
                }
                if (!widthset || width > 0x15d) width = 0x15d;
                while (isctype_inl(ch, CRT_DIGIT)) {
                    const int w0 = width--;
                    if (!w0) break;
                    *p++ = (char)ch;
                    started++;
                    charcount++;
                    ch = c_inc(stream);
                }
                if ((uint8_t)crt_decimal_point == (uint8_t)ch) {
                    const int w0 = width--;
                    if (w0) {
                        charcount++;
                        p++;
                        ch = c_inc(stream);
                        p[-1] = crt_decimal_point;
                        while (isctype_inl(ch, CRT_DIGIT)) {
                            const int w1 = width--;
                            if (!w1) break;
                            *p++ = (char)ch;
                            started++;
                            charcount++;
                            ch = c_inc(stream);
                        }
                    }
                }
                if (started && (ch == 'e' || ch == 'E')) {
                    const int w0 = width--;
                    if (w0) {
                        *p++ = 'e';
                        charcount++;
                        ch = c_inc(stream);
                        if (ch == '-') {
                            *p++ = '-';
                            goto f_incwidth2;
                        } else if (ch == '+') {
                        f_incwidth2: {
                                const int w1 = width--;
                                if (!w1) {
                                    width++;
                                    goto f_expdigits;
                                }
                            }
                            charcount++;
                            ch = c_inc(stream);
                        }
                    f_expdigits:
                        while (isctype_inl(ch, CRT_DIGIT)) {
                            const int w1 = width--;
                            if (!w1) break;
                            *p++ = (char)ch;
                            started++;
                            charcount++;
                            ch = c_inc(stream);
                        }
                    }
                }
                charcount--;
                c_un_inc(ch, stream);
                if (!started) goto error_return_no_unget;
                if (!suppress) {
                    count++;
                    *p = 0;
                    if (dry) dry->store(pointer, longone - 1 ? 8 : 4);
                    else p_fassign((int)longone - 1, pointer, floatstring);
                }
                break;
            }
            case 'i':
                comchr = 'd';
                // fall through
            case 'x':
                if (ch == '-') {
                    negative = 1;
                    goto x_incwidth;
                } else if (ch == '+') {
                x_incwidth:
                    if (!--width && widthset) {
                        done_flag++;
                    } else {
                        charcount++;
                        ch = c_inc(stream);
                    }
                }
                if (ch == '0') {
                    charcount++;
                    ch = c_inc(stream);
                    if ((char)ch == 'x' || (char)ch == 'X') {
                        charcount++;
                        ch = c_inc(stream);
                        comchr = 'x';
                    } else {
                        started = 1;
                        if (comchr != 'x') {
                            comchr = 'o';
                        } else {
                            charcount--;
                            c_un_inc(ch, stream);
                            ch = '0';
                        }
                    }
                }
                goto getnum;
            case 'p':
                longone = 1;
                // fall through
            case 'o':
            case 'u':
            case 'd':
                if (ch == '-') {
                    negative = 1;
                    goto d_incwidth;
                } else if (ch == '+') {
                d_incwidth:
                    if (!--width && widthset) {
                        done_flag++;
                    } else {
                        charcount++;
                        ch = c_inc(stream);
                    }
                }
            getnum:
                if (integer64) {
                    while (!done_flag) {
                        if (comchr == 'x') {
                            if (isctype_inl(ch, CRT_HEX)) {
                                num64 <<= 4;
                                ch = c_hextodec(ch);
                            } else {
                                done_flag++;
                            }
                        } else if (isctype_inl(ch, CRT_DIGIT)) {
                            if (comchr == 'o') {
                                if (ch < '8') num64 <<= 3;
                                else done_flag++;
                            } else {
                                num64 = c_allmul(10, num64);
                            }
                        } else {
                            done_flag++;
                        }
                        if (!done_flag) {
                            started++;
                            num64 += (uint64_t)(int64_t)(ch - '0');
                            if (widthset && !--width) {
                                done_flag++;
                            } else {
                                charcount++;
                                ch = c_inc(stream);
                            }
                        } else {
                            charcount--;
                            c_un_inc(ch, stream);
                        }
                    }
                    if (negative) num64 = 0 - num64;
                } else {
                    while (!done_flag) {
                        if (comchr == 'x' || comchr == 'p') {
                            if (isctype_inl(ch, CRT_HEX)) {
                                number <<= 4;
                                ch = c_hextodec(ch);
                            } else {
                                done_flag++;
                            }
                        } else if (isctype_inl(ch, CRT_DIGIT)) {
                            if (comchr == 'o') {
                                if (ch < '8') number <<= 3;
                                else done_flag++;
                            } else {
                                number = number * 10;
                            }
                        } else {
                            done_flag++;
                        }
                        if (!done_flag) {
                            started++;
                            number = number + (U32)ch - '0';
                            if (widthset && !--width) {
                                done_flag++;
                            } else {
                                charcount++;
                                ch = c_inc(stream);
                            }
                        } else {
                            charcount--;
                            c_un_inc(ch, stream);
                        }
                    }
                    if (negative) number = 0u - number;
                }
                if (comchr == 'F') started = 0;                       // (never: comchr is lower case)
                if (!started) goto error_return_no_unget;
                if (!suppress) {
                    count++;
                    goto assign_num;
                }
                break;
            case 'n':
                number = (U32)charcount;
                if (!suppress) goto assign_num;
                break;
            assign_num:
                if (integer64) {
                    if (dry) dry->store(pointer, 8);
                    else memcpy(pointer, &num64, 8);
                } else if (longone) {
                    if (dry) dry->store(pointer, 4);
                    else memcpy(pointer, &number, 4);
                } else {
                    if (dry) dry->store(pointer, 2);
                    else *(uint16_t*)pointer = (uint16_t)number;
                }
                break;
            default:
                if ((int)*format != ch) goto error_return;
                match--;
                if (!suppress) arglist = arglistsave;
                break;
            }
            match++;
            format++;
        } else {
            const uint8_t* fc = format++;
            charcount++;
            ch = c_inc(stream);
            if ((int)*fc != ch) goto error_return;
            if (crt_pctype[(uint8_t)ch] & CRT_LEADBYTE) {
                const uint8_t* fc2 = format++;
                charcount++;
                const int ch2 = c_inc(stream);
                if ((int)*fc2 != ch2) {
                    charcount--;
                    c_un_inc(ch2, stream);
                    goto error_return;
                }
                charcount--;
            }
        }
        if (ch == -1 && (*format != '%' || format[1] != 'n')) break;
    }
    goto done;
error_return:
    charcount--;
    c_un_inc(ch, stream);
error_return_no_unget:
done:
    if (dry) dry->flush();
    if (ch == -1) return (count || match) ? count : -1;
    return count;
}

static int __cdecl crt_input(CrtFile* stream, const uint8_t* format, char* arglist) {
    return input_engine(stream, format, arglist, 0);
}
static void fp_input(Footprint& f, CrtFile*, const uint8_t*, char*) { f.replay_only = "reads a stream (_filbuf: file I/O)"; }
PORT_FN(0x004d0ee0, "input", crt_input, fp_input)

// =====================================================================================================================
// sprintf.obj, vsprintf.obj, sscanf.obj, printf.obj, fprintf.obj
// =====================================================================================================================
// what printf may write besides its output: cvt.obj's and cfout.obj's statics (%e / %f / %g) and errno (wctomb)
static void fp_printf_statics(Footprint& f) {
    f.add((void*)(uintptr_t)0x005d5730, 0x1a, "_fltout's FOS");
    f.add((void*)(uintptr_t)0x005d5750, 0x10, "_fltout's STRFLT");
    f.add((void*)(uintptr_t)0x00502710, 12, "cvt.obj's %g state");
    f.add((void*)(uintptr_t)0x005d55f8, 4, "cvt.obj's STRFLT*");
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno");
}
// the string a sprintf will write: its length from a dry run of the engine (no output, %n stores nothing)
static void fp_sprintf_buffer(Footprint& f, char* string, const char* format, char* argptr) {
    fp_printf_statics(f);
    OutDry dry = {0};
    output_engine(0, format, argptr, &dry);
    f.add(string, (uint32_t)dry.count + 1, "sprintf's buffer");
}

// sprintf (0x4cf0a0): through _output into a string "stream" (_IOWRT | _IOSTRG, a count of 0x7fffffff), terminated
static int __cdecl crt_sprintf(char* string, const char* format, Va va) {
    CrtFile str;
    str.flag = CRT_IOWRT | CRT_IOSTRG;
    str.ptr = str.base = string;
    str.cnt = 0x7fffffff;
    const int retval = c_output(&str, format, (char*)&va);
    if (--str.cnt >= 0) *str.ptr++ = 0;
    else c_flsbuf(0, &str);
    return retval;
}
static void fp_sprintf(Footprint& f, char* string, const char* format, Va va) { fp_sprintf_buffer(f, string, format, (char*)&va); }
PORT_FN(0x004cf0a0, "sprintf", crt_sprintf, fp_sprintf)

// vsprintf (0x4cf7c0)
static int __cdecl crt_vsprintf(char* string, const char* format, char* ap) {
    CrtFile str;
    str.flag = CRT_IOWRT | CRT_IOSTRG;
    str.ptr = str.base = string;
    str.cnt = 0x7fffffff;
    const int retval = c_output(&str, format, ap);
    if (--str.cnt >= 0) *str.ptr++ = 0;
    else c_flsbuf(0, &str);
    return retval;
}
static void fp_vsprintf(Footprint& f, char* string, const char* format, char* ap) { fp_sprintf_buffer(f, string, format, ap); }
PORT_FN(0x004cf7c0, "vsprintf", crt_vsprintf, fp_vsprintf)

// sscanf (0x4ce190): through _input from a string "stream" (_IOREAD | _IOSTRG | _IOMYBUF, the string's length)
static int __cdecl crt_sscanf(const char* string, const char* format, Va va) {
    CrtFile str;
    str.flag = CRT_IOREAD | CRT_IOSTRG | CRT_IOMYBUF;
    str.ptr = str.base = (char*)string;
    str.cnt = (int32_t)strlen(string);
    return c_input(&str, (const uint8_t*)format, (char*)&va);
}
static void fp_sscanf(Footprint& f, const char* string, const char* format, Va va) {
    // the destinations: a dry run over a copy of the stream
    CrtFile str;
    str.flag = CRT_IOREAD | CRT_IOSTRG | CRT_IOMYBUF;
    str.ptr = str.base = (char*)string;
    str.cnt = (int32_t)strlen(string);
    InDry dry = {&f, 0, 0};
    input_engine(&str, (const uint8_t*)format, (char*)&va, &dry);
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno");                // (mbtowc, _fassign's conversions: never set, listed)
}
PORT_FN(0x004ce190, "sscanf", crt_sscanf, fp_sscanf)

// printf (0x4d01c0) / fprintf (0x4d02a0): a temporary buffer on a console stream (_stbuf / _ftbuf) around _output
static int __cdecl crt_printf(const char* format, Va va) {
    CrtFile* const out = (CrtFile*)(uintptr_t)CRT_STDOUT_VA;
    const int buffing = c_stbuf(out);
    const int retval = c_output(out, format, (char*)&va);
    c_ftbuf(buffing, out);
    return retval;
}
static void fp_printf(Footprint& f, const char*, Va) { f.replay_only = "writes stdout (file I/O)"; }
PORT_FN(0x004d01c0, "printf", crt_printf, fp_printf)

static int __cdecl crt_fprintf(CrtFile* stream, const char* format, Va va) {
    const int buffing = c_stbuf(stream);
    const int retval = c_output(stream, format, (char*)&va);
    c_ftbuf(buffing, stream);
    return retval;
}
static void fp_fprintf(Footprint& f, CrtFile*, const char*, Va) { f.replay_only = "writes a stream (file I/O)"; }
PORT_FN(0x004d02a0, "fprintf", crt_fprintf, fp_fprintf)
