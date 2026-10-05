// crt_types.h -- the game's statically linked C runtime (Microsoft LIBC.lib, single-threaded, ~1997 MSVC 5), as the
// v1.0 race.exe lays it out: the shared structures and the CRT's global state, at their v1.0 addresses, for the M3
// stage LIBC rewrites (crt_fmt.cpp, crt_float.cpp -- group A; crt_io.cpp, crt_heap.cpp, crt_str.cpp, crt_fdiv.cpp --
// group B). The deferred start-up code (wincrt0, crt0, ioinit, heapinit ...) and the original code still running share
// every one of these, so the rewrites use them in place and never keep copies.
//
// Each group adds what it needs to its own section; the shared basics (FILE, errno, ctype, locale) are at the top.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CRT_G(T, va) (*(T volatile*)(uintptr_t)(va))          // a CRT global at its v1.0 address
#define CRT_P(T, va) ((T*)(uintptr_t)(va))                      // ... and its address

// ---- streams (stdio.h, _file.obj) ---------------------------------------------------------------------------------
struct CrtFile {                       // FILE, 0x20 bytes
    char* ptr;                         // +0x00 next character
    int32_t cnt;                       // +0x04 characters left in the buffer
    char* base;                        // +0x08 the buffer
    int32_t flag;                      // +0x0c _IO* flags
    int32_t file;                      // +0x10 the handle (a lowio file number)
    int32_t charbuf;                   // +0x14 the one-character buffer (_IONBF)
    int32_t bufsiz;                    // +0x18
    char* tmpfname;                    // +0x1c
};
static_assert(sizeof(CrtFile) == 0x20 && offsetof(CrtFile, flag) == 0x0c && offsetof(CrtFile, bufsiz) == 0x18,
              "CrtFile");
enum {
    CRT_IOREAD = 0x0001, CRT_IOWRT = 0x0002, CRT_IONBF = 0x0004, CRT_IOMYBUF = 0x0008, CRT_IOEOF = 0x0010,
    CRT_IOERR = 0x0020, CRT_IOSTRG = 0x0040, CRT_IORW = 0x0080, CRT_IOYOURBUF = 0x0100, CRT_IOSETVBUF = 0x0400,
    CRT_IOFEOF = 0x0800, CRT_IOFLRTN = 0x1000, CRT_IOCTRLZ = 0x2000, CRT_IOCOMMIT = 0x4000,
};
#define CRT_IOB_VA          0x00503430u                 // _iob[20]: stdin, stdout, stderr, then fopen's
#define CRT_IOB             CRT_P(CrtFile, CRT_IOB_VA)
#define CRT_STDOUT_VA       0x00503450u
#define CRT_STDERR_VA       0x00503470u
#define CRT_STDBUF_VA       0x00503428u                 // _stdbuf[2]: stdout's and stderr's temporary buffers
#define CRT_CFLUSH_VA       0x005036b0u                 // _cflush
#define CRT_NSTREAM_VA      0x005d5dfcu                 // _nstream
#define CRT_PIOB_VA         0x005d5e00u                 // __piob (FILE**)

// ---- low-level I/O (ioinfo: osfhnd + osfile, 32 per block) -------------------------------------------------------------
struct CrtIoinfo { intptr_t osfhnd; uint8_t osfile; uint8_t pipech; uint8_t _6[2]; };
static_assert(sizeof(CrtIoinfo) == 8, "CrtIoinfo");
#define CRT_NHANDLE_VA      0x005d6e14u                 // _nhandle
#define CRT_PIOINFO_VA      0x005d6e20u                 // __pioinfo[64] (CrtIoinfo*, 32 each)
#define CRT_BADIOINFO_VA    0x00502d18u                 // __badioinfo (for handle -1)
#define CRT_FMODE_VA        0x0050379cu                 // _fmode
#define CRT_COMMODE_VA      0x00503740u                 // _commode
#define CRT_UMASKVAL_VA     0x00502738u                 // _umaskval

// ---- errors ------------------------------------------------------------------------------------------------------------
#define CRT_ERRNO_VA        0x00502730u
#define CRT_DOSERRNO_VA     0x00502734u
#define crt_errno           CRT_G(int32_t, CRT_ERRNO_VA)
#define crt_doserrno        CRT_G(uint32_t, CRT_DOSERRNO_VA)
enum { CRT_EBADF = 9, CRT_ENOMEM = 12, CRT_EACCES = 13, CRT_EINVAL = 22, CRT_EMFILE = 24, CRT_ERANGE = 34 };

// ---- ctype and the locale ----------------------------------------------------------------------------------------------
#define CRT_PCTYPE_VA       0x00503078u                 // _pctype (const uint16_t*, = _ctype + 1 = 0x503082)
#define CRT_CTYPE_VA        0x00503080u                 // _ctype[257] (EOF first)
#define CRT_MB_CUR_MAX_VA   0x00503284u                 // __mb_cur_max
#define CRT_DECIMAL_VA      0x00503288u                 // __decimal_point (a char; the "C" locale's '.')
#define CRT_MBCTYPE_VA      0x00502b00u                 // _mbctype[257]
#define crt_pctype          CRT_G(const uint16_t*, CRT_PCTYPE_VA)
#define crt_mb_cur_max      CRT_G(int32_t, CRT_MB_CUR_MAX_VA)
#define crt_decimal_point   CRT_G(char, CRT_DECIMAL_VA)
enum {                                  // _ctype bits
    CRT_UPPER = 0x01, CRT_LOWER = 0x02, CRT_DIGIT = 0x04, CRT_SPACE = 0x08, CRT_PUNCT = 0x10, CRT_CONTROL = 0x20,
    CRT_BLANK = 0x40, CRT_HEX = 0x80, CRT_LEADBYTE = 0x8000, CRT_ALPHA = 0x0103,
};

// ---- floating point (fpinit.obj, cmiscdat.obj) -----------------------------------------------------------------------
#define CRT_FASTFLAG_VA     0x005024b4u                 // __fastflag
#define CRT_ADJUST_FDIV_VA  0x005024b8u                 // _adjust_fdiv (the Pentium FDIV workaround's switch)
#define CRT_MATHERR_FLAG_VA 0x005036ecu                 // _matherr_flag
// _cfltcvt_tab[6]: the float conversions printf / scanf reach through pointers (_cfltcvt_init fills them; until then
// each is _fptrap, "floating point not loaded")
#define CRT_CFLTCVT_VA      0x005026f8u                 // _cfltcvt   (double*, char*, int fmt, int prec, int caps)
#define CRT_CROPZEROS_VA    0x005026fcu                 // _cropzeros (char*)
#define CRT_FASSIGN_VA      0x00502700u                 // _fassign   (int flag, char* arg, char* text)
#define CRT_FORCDECPT_VA    0x00502704u                 // _forcdecpt (char*)
#define CRT_POSITIVE_VA     0x00502708u                 // _positive  (double*)
#define CRT_CFLTCVT2_VA     0x0050270cu                 // _cfltcvt again

// 80-bit reals as the CRT's conversion code keeps them
struct CrtLd12 { uint8_t b[12]; };     // _LDBL12: 96-bit mantissa (bytes 0..9), sign | exponent in bytes 10..11
struct CrtLd10 { uint8_t b[10]; };     // _LDOUBLE: x87 extended
struct CrtStrflt {                     // STRFLT (cfout.c): what _fltout returns
    int32_t sign;                      // '-' or ' '
    int32_t decpt;                     // the decimal exponent
    int32_t flag;                      // 1: the digits are good (x10fout's return)
    char* mantissa;                    // the digits
};
struct CrtFos {                        // FOS (x10fout's output)
    int16_t exp;
    char sign;
    char ManLen;
    char man[22];                      // MAX_MAN_DIGITS + 1
};
static_assert(sizeof(CrtFos) == 0x1a, "CrtFos");
struct CrtFlt {                        // FLT (cfin.c): what _fltin returns
    int32_t flags;
    int32_t nbytes;
    int32_t lval;
    int32_t _0c;
    double dval;                       // +0x10
};
static_assert(sizeof(CrtFlt) == 0x18 && offsetof(CrtFlt, dval) == 0x10, "CrtFlt");

// ---- group B: files, heap, strings, misc (crt_io.cpp, crt_heap.cpp, crt_str.cpp, crt_fdiv.cpp) adds below ----------
#define CRT_CRTHEAP_VA      0x005d6e10u                 // _crtheap (HANDLE)
#define CRT_NEWMODE_VA      0x005036e8u                 // _newmode (malloc calls the new handler when set)
#define CRT_APP_TYPE_VA     0x00502690u                 // __app_type (1 console, 2 GUI: the game is 2)
#define CRT_EXITFLAG_VA     0x00502770u                 // _exitflag (a char: __endstdio closes every stream when set)
#define CRT_LC_HANDLE_VA    0x00503408u                 // __lc_handle[6]: [LC_CTYPE] at 0x503410 (0: the "C" locale)
#define CRT_LC_CTYPE_VA     0x00503410u
#define CRT_LC_CODEPAGE_VA  0x00503420u                 // __lc_codepage
#define CRT_MBCODEPAGE_VA   0x00502c04u                 // __mbcodepage
#define CRT_MBLCID_VA       0x00502c08u                 // __mblcid
#define CRT_MBULINFO_VA     0x00502c10u                 // __mbulinfo[3]
#define CRT_FSYSTEMSET_VA   0x00502c1cu                 // fSystemSet (mbctype.obj)
#define CRT_RGCTYPEFLAG_VA  0x00502c20u                 // __rgctypeflag[4] (mbctype.obj)
#define CRT_CPINFO_TAB_VA   0x00502c28u                 // __rgcode_page_info[5]: {int cp; uint16 mbulinfo[6]; uint8 rgrange[4][8]}
#define CRT_CPINFO_TAB_END  0x00502d18u
#define CRT_AWMAP_FUSE_VA   0x005033fcu                 // __crtLCMapStringA's f_use (0 untested, 1 W, 2 A)
#define CRT_AWSTR_FUSE_VA   0x0050373cu                 // __crtGetStringTypeA's f_use
#define CRT_EMPTY_A_VA      0x00503400u                 // "" (the probes' one-character string)
#define CRT_EMPTY_W_VA      0x00503404u                 // L""
#define CRT_DOSERRTAB_VA    0x00503290u                 // dosmaperr's {os code, errno} table, to 0x5033f8
#define CRT_DOSERRTAB_END   0x005033f8u
#define CRT_FDIV_NIBBLE_VA  0x005024d0u                 // adj_fdiv: the 16 flagged divisor nibbles (byte table)
#define CRT_FPREM_NIBBLE_VA 0x005024ecu                 // adj_fdiv: the same for fprem
#define crt_lc_ctype        CRT_G(uint32_t, CRT_LC_CTYPE_VA)
#define crt_lc_codepage     CRT_G(uint32_t, CRT_LC_CODEPAGE_VA)
#define crt_crtheap         CRT_G(void*, CRT_CRTHEAP_VA)
#define crt_newmode         CRT_G(int32_t, CRT_NEWMODE_VA)
#define crt_nhandle         CRT_G(uint32_t, CRT_NHANDLE_VA)
#define crt_nstream         CRT_G(int32_t, CRT_NSTREAM_VA)
#define crt_piob            CRT_G(CrtFile**, CRT_PIOB_VA)
#define crt_cflush          CRT_G(int32_t, CRT_CFLUSH_VA)
enum {                                  // ioinfo.osfile bits
    CRT_FOPEN = 0x01, CRT_FEOFLAG = 0x02, CRT_FCRLF = 0x04, CRT_FPIPE = 0x08, CRT_FNOINHERIT = 0x10, CRT_FAPPEND = 0x20,
    CRT_FDEV = 0x40, CRT_FTEXT = 0x80,
};
enum { CRT_ENOSPC = 28, CRT_EILSEQ = 42, CRT_ENOEXEC = 8 };
// A lowio handle's ioinfo, with the 1998 compiler's own arithmetic: the block pointer at 0x5d6e20 + ((fh & ~0x18) >> 3)
// (sar: a handle of -1 reads the dword just below the table, as the original does where it doesn't check), then the
// slot (fh & 0x1f). Every original reads the block pointer afresh at the point it uses it; so does this.
static __forceinline CrtIoinfo* crt_ioinfo(int32_t fh) {
    const int32_t off = (int32_t)((uint32_t)fh & 0xffffffe7u) >> 3;
    CrtIoinfo* blk = *(CrtIoinfo* volatile*)(uintptr_t)(CRT_PIOINFO_VA + (uint32_t)off);
    return blk + ((uint32_t)fh & 0x1f);
}
#define crt_osfile(fh)      (crt_ioinfo(fh)->osfile)

// ---- group C (stage S1): start-up, exit and exception plumbing (crt_start.cpp) ----------------------------------------
#define CRT_OSVER_VA                0x0050273cu         // _osver, then _winver 0x502740, _winmajor 0x502744, _winminor 0x502748
#define CRT_ACMDLN_VA               0x005d6f28u         // _acmdln (GetCommandLineA's string)
#define CRT_AENVPTR_VA              0x00502680u         // _aenvptr (__crtGetEnvironmentStringsA's block; freed by _setenvp)
#define CRT_AW_ENV_FUSE_VA          0x00502af8u         // __crtGetEnvironmentStringsA's f_use (1 wide, 2 ANSI)
#define CRT_ARGC_VA                 0x0050274cu         // __argc
#define CRT_ARGV_VA                 0x00502750u         // __argv
#define CRT_ENVIRON_VA              0x00502758u         // _environ
#define CRT_PGMPTR_VA               0x00502768u         // _pgmptr (-> _pgmname)
#define CRT_PGMNAME_VA              0x005d5610u         // _pgmname[260]
#define CRT_ONEXITBEGIN_VA          0x005d6f24u         // __onexitbegin
#define CRT_ONEXITEND_VA            0x005d6f20u         // __onexitend
#define CRT_C_TERMINATION_DONE_VA   0x00502774u         // _C_Termination_Done
#define CRT_FPINIT_VA               0x005024bcu         // _FPinit (-> _fpmath)
#define CRT_ERROR_MODE_VA           0x0050268cu         // __error_mode
#define CRT_AEXIT_RTN_VA            0x00502688u         // _aexit_rtn (-> _exit)
#define CRT_ADBGMSG_VA              0x00503010u         // _adbgmsg (0)
#define CRT_RTERRS_VA               0x00502f88u         // rterrs[17]: {number, text}
#define CRT_RTERRS_END              0x00503010u
#define CRT_STR_VCRT_VA             0x00503014u         // "Microsoft Visual C++ Runtime Library"
#define CRT_STR_NLNL_VA             0x0050303cu         // "\n\n"
#define CRT_STR_RUNTIMEERR_VA       0x00503040u         // "Runtime Error!\n\nProgram: " (26 bytes with the NUL)
#define CRT_STR_DOTS_VA             0x0050305cu         // "..."
#define CRT_STR_PROGUNKNOWN_VA      0x00503060u         // "<program name unknown>" (23 bytes)
#define CRT_PFN_MESSAGEBOX_VA       0x005036f0u         // __crtMessageBoxA's user32 pointers
#define CRT_PFN_GETACTIVEWINDOW_VA  0x005036f4u
#define CRT_PFN_GETLASTACTIVEPOPUP_VA 0x005036f8u
#define CRT_STR_GETLASTACTIVEPOPUP_VA 0x005036fcu
#define CRT_STR_GETACTIVEWINDOW_VA  0x00503710u
#define CRT_STR_MESSAGEBOXA_VA      0x00503720u
#define CRT_STR_USER32_VA           0x0050372cu
#define CRT_PNHHEAP_VA              0x005d5760u         // _pnhHeap (the new handler)
#define CRT_XCPTACTTAB_VA           0x00502a68u         // _XcptActTab[]: {exception code, signal, action}
#define CRT_FIRST_FPE_INDX_VA       0x00502ae0u
#define CRT_NUM_FPE_VA              0x00502ae4u
#define CRT_XCPTACTTABCOUNT_VA      0x00502ae8u
#define CRT_FPECODE_VA              0x00502aecu         // _fpecode
#define CRT_PXCPTINFOPTRS_VA        0x00502af0u         // _pxcptinfoptrs
#define CRT_NLG_DESTINATION_VA      0x00502a58u         // __NLG_Destination {signature, destination, code, ebp}
#define CRT_D_INF_VA                0x005028a0u         // _d_inf (double)
#define CRT_D_MAX_VA                0x005028b0u         // _d_max (double)
// the initialiser / terminator tables _cinit and doexit run (_initterm: [a, z))
#define CRT_XI_A                    0x004e3738u         // C initialisers: __onexitinit, __initstdio
#define CRT_XI_Z                    0x004e3744u
#define CRT_XC_A                    0x004e1000u         // C++ initialisers: the game's $E functions
#define CRT_XC_Z                    0x004e3734u
#define CRT_XP_A                    0x004e3748u         // pre-terminators: __endstdio
#define CRT_XP_Z                    0x004e3750u
#define CRT_XT_A                    0x004e3754u         // terminators: none
#define CRT_XT_Z                    0x004e3758u

// The islands (crt_start.cpp): code the game still reaches that can't take a 5-byte hook jump at its own address -- 87disp's
// result routines (reached through the transcendental tables and crt_float.cpp's 87triga rewrites), __adj_fpatan
// (0x4cef65) and __rdtsc (0x4188ad, ProfBegin's rewrite on a one-processor machine with a TSC). Each entry is a 5-byte jmp to its
// rewrite, or a 2-byte short jmp to a 5-byte jmp placed in dead bytes nearby (padding, or 87disp's unreferenced
// routines). crt_islands_install() writes them, every byte checked first (v1.0's, int3, or already its own; per group,
// all or nothing; idempotent): _WinMainCRTStartup's rewrite calls it first thing, in both routes. A standalone int3
// fill must leave these bytes to it (they are written after the fill, at start-up) -- or call it after the fill.
struct CrtIsland { uint32_t at; uint32_t len; uint32_t target; const char* what; };   // target: the rewrite, or the slot
int crt_islands(const CrtIsland** list);       // the table (18 patches)
bool crt_islands_install();                    // false if a group's bytes weren't v1.0's (logged; that group left alone)
