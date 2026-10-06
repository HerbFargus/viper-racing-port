// crt_io.cpp -- M3 stage LIBC, group B: the game's C runtime files, rewritten. LIBC objects: the low-level I/O (open:
// _sopen, read, write, close, lseek, commit, chsize, setmode, isatty, osfinfo: the handle table, dosmap), the streams
// (_open: _openfile, fopen: _fsopen / fopen, fclose, fread, fwrite, fseek, ftell, fflush: fflush / _flush / _flushall /
// flsall, _flsbuf, _filbuf, _getbuf, _freebuf, _sftbuf: _stbuf / _ftbuf, stream: _getstream, _file: __initstdio /
// __endstdio, ungetc) and the directories (getcwd: _getcwd / _getdcwd / _validdrive, chdir, fullpath).
//
// Written from the v1.0 disassembly (tools/disasm.py), faithful: every call in the original's order and by its v1.0
// address (this group's own too: a hooked rewrite, or the deferred original -- _amsg_exit, _fcloseall -- is what
// runs), Windows through the game's import slots, read at the call; the CRT's state at its v1.0 addresses
// (crt_types.h): _iob, __piob / _nstream, the ioinfo blocks (__pioinfo, _nhandle), errno / _doserrno, _fmode,
// _commode, _umaskval, _cflush, _stdbuf. Each original reads a FILE's _flag once and tests that copy where it does,
// and re-reads it where it does; so does this. Memory the originals move with rep movsd / movsb (fread's and fwrite's
// buffer copies, _getdcwd's strcpy) is moved the same way.
//
// Text mode is the original's to the byte: _read turns CR LF into LF in place, stops at ^Z (and marks the handle at
// end of file unless it's a device), and for a CR that ends the buffer reads one more byte to decide (putting it back
// with lseek(-1) on a file, in the pipe character on a device or pipe); _write expands LF to CR LF through a
// 1024-byte buffer (which can reach 1025: the original's array is 1025); ftell counts the LFs back. _sopen on a text
// file opened for reading and writing chops a final ^Z (_chsize).
//
// Quirks kept (faithful):
//   * ftell indexes the ioinfo table with stream->_file unchecked (-1 reads the dword below __pioinfo); _flsbuf and
//     _filbuf use __badioinfo for -1.
//   * flsall walks 512 __piob entries whatever _nstream is (_nstream is 512 unless the first calloc failed).
//   * _fullpath leaks the buffer it allocated when GetFullPathName fails or doesn't fit.
//   * fwrite hands _flsbuf its character sign-extended (movsx), and treats a _bufsiz of 0 or less as 1.
//   * _close of handle 1 or 2 when both are the same OS handle doesn't close it; _commit and _chsize set errno but
//     not _doserrno for a bad handle.
// Every function here does file or directory I/O, allocates, or is reached only through those, so its footprint is
// replay_only, except the table helpers (_dosmaperr, _isatty, _setmode, the osfhnd functions, ungetc on a buffered
// stream), which are shadow-checked. test/world_crt_io.cpp checks all of them offline against the originals on a
// sandbox directory.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "crt_types.h"

namespace crt_io {   // (file-local helpers; one namespace per file so a harness can include all four)

// ---- the game's imports and functions -------------------------------------------------------------------------------
#define IAT(slot, fn) (*(decltype(&fn) volatile*)(uintptr_t)(slot))
#define kSetCurrentDirectoryA     IAT(0x005d7598, SetCurrentDirectoryA)
#define kGetCurrentDirectoryA     IAT(0x005d75b0, GetCurrentDirectoryA)
#define kSetEnvironmentVariableA  IAT(0x005d75a8, SetEnvironmentVariableA)
#define kGetLastError             IAT(0x005d7540, GetLastError)
#define kGetFullPathNameA         IAT(0x005d7594, GetFullPathNameA)
#define kGetDriveTypeA            IAT(0x005d75ec, GetDriveTypeA)
#define kSetFilePointer           IAT(0x005d754c, SetFilePointer)
#define kReadFile                 IAT(0x005d753c, ReadFile)
#define kWriteFile                IAT(0x005d7538, WriteFile)
#define kCloseHandle              IAT(0x005d7548, CloseHandle)
#define kSetStdHandle             IAT(0x005d7600, SetStdHandle)
#define kFlushFileBuffers         IAT(0x005d75e0, FlushFileBuffers)
#define kCreateFileA              IAT(0x005d7554, CreateFileA)
#define kGetFileType              IAT(0x005d75e8, GetFileType)
#define kSetEndOfFile             IAT(0x005d75f4, SetEndOfFile)

template <typename R, typename... A> static __forceinline R gcall(uint32_t fn, A... a) {
    typedef R(__cdecl * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}
enum : uint32_t {
    F_malloc = 0x004d1de0, F_free = 0x004d58b0, F_calloc = 0x004d7170, F_amsg_exit = 0x004cf730,
    F_mbctoupper = 0x004d48b0, F_dosmaperr = 0x004d4840, F_getcwd = 0x004d4940, F_getdcwd = 0x004d4960,
    F_validdrive = 0x004d4a90, F_ftell = 0x004d02e0, F_flush = 0x004d51d0, F_lseek = 0x004d50c0, F_read = 0x004d53b0,
    F_filbuf = 0x004d52c0, F_getstream = 0x004d5830, F_openfile = 0x004d5620, F_fsopen = 0x004d05e0,
    F_freebuf = 0x004d59a0, F_close = 0x004d58d0, F_write = 0x004d59e0, F_flsbuf = 0x004d2160, F_isatty = 0x004d6680,
    F_getbuf = 0x004d6630, F_flsall = 0x004d5250, F_fflush = 0x004d5180, F_commit = 0x004d74a0,
    F_flushall = 0x004d5240, F_fcloseall = 0x004d71d0, F_get_osfhandle = 0x004d7450, F_free_osfhnd = 0x004d73c0,
    F_alloc_osfhnd = 0x004d7250, F_set_osfhnd = 0x004d7310, F_sopen = 0x004d7510, F_chsize = 0x004d8210,
    F_setmode = 0x004d83c0,
};
#define G32(va) CRT_G(uint32_t, va)

static __forceinline CrtIoinfo* io_safe(int32_t fh) {    // _osfile_safe: __badioinfo for -1
    return fh == -1 ? (CrtIoinfo*)(uintptr_t)CRT_BADIOINFO_VA : crt_ioinfo(fh);
}
static __forceinline bool fh_bad(int32_t fh) { return (uint32_t)fh >= crt_nhandle || !(crt_osfile(fh) & CRT_FOPEN); }
static __forceinline intptr_t osfhandle(int32_t fh) { return gcall<intptr_t>(F_get_osfhandle, fh); }

// rep movsd (n / 4 dwords), then rep movsb (n & 3), forwards: the original's inlined memcpy
static __forceinline void rep_movs(void* d, const void* s, uint32_t n) {
#ifdef VP_GCC
    __asm__ volatile("mov edi, %0\n\t"
                     "mov esi, %1\n\t"
                     "mov ecx, %2\n\t"
                     "%{load%} mov eax, ecx\n\t"
                     "shr ecx, 2\n\t"
                     "rep movsd\n\t"
                     "%{load%} mov ecx, eax\n\t"
                     "and ecx, 3\n\t"
                     "rep movsb"
                     :
                     : "m"(d), "m"(s), "m"(n)
                     : VP_X87_CLOBBERS, "eax", "ecx", "esi", "edi", "cc", "memory");
#else
    __asm {
        mov edi, d
        mov esi, s
        mov ecx, n
        mov eax, ecx
        shr ecx, 2
        rep movsd
        mov ecx, eax
        and ecx, 3
        rep movsb
    }
#endif
}

static void fp_io(Footprint& f) { f.replay_only = "file I/O"; }
#define FP_IO(name, ...) static void name(Footprint& f, __VA_ARGS__) { fp_io(f); }

// =====================================================================================================================
// errors, the handle table: dosmap.obj, osfinfo.obj, isatty.obj, setmode.obj
// =====================================================================================================================
// _dosmaperr (0x4d4840)
static void __cdecl c_dosmaperr(uint32_t e) {
    crt_doserrno = e;
    for (uint32_t t = CRT_DOSERRTAB_VA; t < CRT_DOSERRTAB_END; t += 8)
        if (G32(t) == e) {
            crt_errno = (int32_t)G32(t + 4);
            return;
        }
    if (e >= 0x13 && e <= 0x24) crt_errno = CRT_EACCES;
    else if (e >= 0xbc && e <= 0xca) crt_errno = CRT_ENOEXEC;
    else crt_errno = CRT_EINVAL;
}
static void fp_errnos(Footprint& f) {
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 8, "errno, _doserrno");
}
static void fp_dosmaperr(Footprint& f, uint32_t) { fp_errnos(f); }
PORT_FN(0x004d4840, "dosmaperr", c_dosmaperr, fp_dosmaperr)

// _get_osfhandle (0x4d7450)
static intptr_t __cdecl c_get_osfhandle(int32_t fh) {
    if ((uint32_t)fh < crt_nhandle) {
        CrtIoinfo* io = crt_ioinfo(fh);
        if (io->osfile & CRT_FOPEN) return io->osfhnd;
    }
    crt_errno = CRT_EBADF;
    crt_doserrno = 0;
    return -1;
}
static void fp_get_osfhandle(Footprint& f, int32_t) { fp_errnos(f); }
PORT_FN(0x004d7450, "get_osfhandle", c_get_osfhandle, fp_get_osfhandle)

// _isatty (0x4d6680): FDEV
static int32_t __cdecl c_isatty(int32_t fh) {
    if (crt_nhandle <= (uint32_t)fh) return 0;
    return crt_osfile(fh) & CRT_FDEV;
}
static void fp_isatty(Footprint&, int32_t) {}
PORT_FN(0x004d6680, "isatty", c_isatty, fp_isatty)

// _setmode (0x4d83c0): the old mode (_O_TEXT 0x4000 / _O_BINARY 0x8000)
static int32_t __cdecl c_setmode(int32_t fh, int32_t mode) {
    if ((uint32_t)fh >= crt_nhandle) {
        crt_errno = CRT_EBADF;
        return -1;
    }
    uint8_t* p = &crt_ioinfo(fh)->osfile;
    uint8_t b = *p;
    if (!(b & CRT_FOPEN)) {
        crt_errno = CRT_EBADF;
        return -1;
    }
    const uint32_t old = b & CRT_FTEXT;
    if (mode == 0x8000) b &= 0x7f;
    else if (mode == 0x4000) b |= 0x80;
    else {
        crt_errno = CRT_EINVAL;
        return -1;
    }
    *p = b;
    return old ? 0x4000 : 0x8000;
}
static void fp_setmode(Footprint& f, int32_t fh, int32_t) {
    if ((uint32_t)fh < crt_nhandle) f.add(&crt_ioinfo(fh)->osfile, 1, "osfile");
    f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno");
}
PORT_FN(0x004d83c0, "setmode", c_setmode, fp_setmode)

// _alloc_osfhnd (0x4d7250): the first free slot (its osfhnd set to -1), a new block of 32 when every block is full
static int32_t __cdecl c_alloc_osfhnd() {
    int32_t fh = -1;
    for (uint32_t i = 0; i < 64; i++) {
        CrtIoinfo* volatile* slot = (CrtIoinfo* volatile*)(uintptr_t)(CRT_PIOINFO_VA + i * 4);
        CrtIoinfo* blk = *slot;
        if (blk) {
            CrtIoinfo* end = blk + 32;
            for (CrtIoinfo* p = blk; p < end; p++)
                if (!(p->osfile & CRT_FOPEN)) {
                    p->osfhnd = -1;
                    fh = (int32_t)(((uint32_t)(uintptr_t)p - (uint32_t)(uintptr_t)*slot) >> 3) + (int32_t)(i * 32);
                    break;
                }
            if (fh != -1) return fh;
        } else {
            CrtIoinfo* n = gcall<CrtIoinfo*>(F_malloc, 0x100u);
            if (n) {
                crt_nhandle += 32;
                *slot = n;
                for (CrtIoinfo* p = n; p < *slot + 32; p++) {
                    p->osfile = 0;
                    p->osfhnd = -1;
                    p->pipech = 10;
                }
                fh = (int32_t)(i << 5);
            }
            return fh;
        }
    }
    return fh;
}
static void fp_alloc_osfhnd(Footprint& f) { f.replay_only = "allocates a block of handles when the table is full"; }
PORT_FN(0x004d7250, "alloc_osfhnd", c_alloc_osfhnd, fp_alloc_osfhnd)

static const DWORD k_std[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

// _set_osfhnd (0x4d7310): a free slot's OS handle (the console's standard handles too, in a console program)
static int32_t __cdecl c_set_osfhnd(int32_t fh, intptr_t value) {
    if ((uint32_t)fh < crt_nhandle && crt_ioinfo(fh)->osfhnd == -1) {
        if (G32(CRT_APP_TYPE_VA) == 1 && (uint32_t)fh < 3) kSetStdHandle(k_std[fh], (HANDLE)value);
        crt_ioinfo(fh)->osfhnd = value;
        return 0;
    }
    crt_errno = CRT_EBADF;
    crt_doserrno = 0;
    return -1;
}
static void fp_osfhnd(Footprint& f, int32_t fh) {
    if ((uint32_t)fh < crt_nhandle) f.add(crt_ioinfo(fh), 8, "ioinfo");
    fp_errnos(f);
}
static void fp_set_osfhnd(Footprint& f, int32_t fh, intptr_t) { fp_osfhnd(f, fh); }
PORT_FN(0x004d7310, "set_osfhnd", c_set_osfhnd, fp_set_osfhnd)

// _free_osfhnd (0x4d73c0)
static int32_t __cdecl c_free_osfhnd(int32_t fh) {
    if ((uint32_t)fh < crt_nhandle) {
        CrtIoinfo* io = crt_ioinfo(fh);
        if ((io->osfile & CRT_FOPEN) && io->osfhnd != -1) {
            if (G32(CRT_APP_TYPE_VA) == 1 && (uint32_t)fh < 3) kSetStdHandle(k_std[fh], 0);
            crt_ioinfo(fh)->osfhnd = -1;
            return 0;
        }
    }
    crt_errno = CRT_EBADF;
    crt_doserrno = 0;
    return -1;
}
static void fp_free_osfhnd(Footprint& f, int32_t fh) { fp_osfhnd(f, fh); }
PORT_FN(0x004d73c0, "free_osfhnd", c_free_osfhnd, fp_free_osfhnd)

// =====================================================================================================================
// low-level I/O: open.obj, read.obj, write.obj, close.obj, lseek.obj, commit.obj, chsize.obj
// =====================================================================================================================
// _lseek (0x4d50c0): clears the handle's end-of-file mark
static int32_t __cdecl c_lseek(int32_t fh, int32_t pos, uint32_t method) {
    if (fh_bad(fh)) {
        crt_errno = CRT_EBADF;
        crt_doserrno = 0;
        return -1;
    }
    const intptr_t h = osfhandle(fh);
    if (h == -1) {
        crt_errno = CRT_EBADF;
        return -1;
    }
    const DWORD np = kSetFilePointer((HANDLE)h, pos, 0, method);
    DWORD err = 0;
    if (np == 0xffffffffu) err = kGetLastError();
    if (err) {
        gcall<void>(F_dosmaperr, err);
        return -1;
    }
    crt_osfile(fh) &= (uint8_t)~CRT_FEOFLAG;
    return (int32_t)np;
}
FP_IO(fp_lseek, int32_t, int32_t, uint32_t)
PORT_FN(0x004d50c0, "lseek", c_lseek, fp_lseek)

// _read (0x4d53b0)
static int32_t __cdecl c_read(int32_t fh, char* buf, uint32_t cnt) {
    if (fh_bad(fh)) {
        crt_errno = CRT_EBADF;
        crt_doserrno = 0;
        return -1;
    }
    int32_t bytes_read = 0;
    char* buffer = buf;
    CrtIoinfo* io = crt_ioinfo(fh);
    if (!cnt || (io->osfile & CRT_FEOFLAG)) return 0;
    if ((io->osfile & (CRT_FPIPE | CRT_FDEV)) && io->pipech != 10) {
        *buffer++ = (char)io->pipech;
        cnt--;
        bytes_read = 1;
        crt_ioinfo(fh)->pipech = 10;
    }
    DWORD os_read;
    if (!kReadFile((HANDLE)crt_ioinfo(fh)->osfhnd, buffer, cnt, &os_read, 0)) {
        const DWORD e = kGetLastError();
        if (e == 5) {
            crt_doserrno = e;
            crt_errno = CRT_EBADF;
            return -1;
        }
        if (e == 0x6d) return 0;
        gcall<void>(F_dosmaperr, e);
        return -1;
    }
    bytes_read += (int32_t)os_read;
    uint8_t* fl = &crt_ioinfo(fh)->osfile;
    uint8_t b = *fl;
    if (!(b & CRT_FTEXT)) return bytes_read;
    if (os_read && *buf == '\n') b |= CRT_FCRLF;
    else b &= (uint8_t)~CRT_FCRLF;
    *fl = b;
    char* p = buf;
    char* q = buf;
    char* const end = buf + bytes_read;
    while (p < end) {
        const char c = *p;
        if (c == 0x1a) {
            uint8_t* f2 = &crt_ioinfo(fh)->osfile;
            const uint8_t v = *f2;
            if (!(v & CRT_FDEV)) *f2 = v | CRT_FEOFLAG;
            break;
        }
        if (c != '\r') {
            p++;
            *q++ = c;
            continue;
        }
        if (p < end - 1) {
            if (p[1] == '\n') {
                p += 2;
                *q++ = '\n';
            } else {
                p++;
                *q++ = c;
            }
            continue;
        }
        p++;
        DWORD err = 0;
        char peek;
        if (!kReadFile((HANDLE)crt_ioinfo(fh)->osfhnd, &peek, 1, &os_read, 0)) err = kGetLastError();
        if (err || !os_read) {
            *q++ = '\r';
        } else if (crt_osfile(fh) & (CRT_FDEV | CRT_FPIPE)) {
            if (peek == '\n') {
                *q++ = '\n';
            } else {
                *q++ = '\r';
                crt_ioinfo(fh)->pipech = (uint8_t)peek;
            }
        } else if (q == buf && peek == '\n') {
            *q++ = '\n';
        } else {
            gcall<int32_t>(F_lseek, fh, -1, 1u);
            if (peek != '\n') *q++ = '\r';
        }
    }
    return (int32_t)(q - buf);
}
FP_IO(fp_read, int32_t, char*, uint32_t)
PORT_FN(0x004d53b0, "read", c_read, fp_read)

// _write (0x4d59e0)
static int32_t __cdecl c_write(int32_t fh, const char* buf, uint32_t cnt) {
    if ((uint32_t)fh >= crt_nhandle) {
        crt_errno = CRT_EBADF;
        crt_doserrno = 0;
        return -1;
    }
    const uint8_t of = crt_osfile(fh);
    if (!(of & CRT_FOPEN)) {
        crt_errno = CRT_EBADF;
        crt_doserrno = 0;
        return -1;
    }
    int32_t lfcount = 0, charcount = 0;
    if (!cnt) return 0;
    if (of & CRT_FAPPEND) gcall<int32_t>(F_lseek, fh, 0, 2u);
    DWORD err, written;
    if (crt_osfile(fh) & CRT_FTEXT) {
        err = 0;
        const char* p = buf;
        while ((uint32_t)(p - buf) < cnt) {
            char lfbuf[0x404];
            char* q = lfbuf;
            while ((uint32_t)(p - buf) < cnt) {
                const char ch = *p++;
                if (ch == '\n') {
                    *q++ = '\r';
                    lfcount++;
                }
                *q++ = ch;
                if (q - lfbuf >= 0x400) break;
            }
            const int32_t n = (int32_t)(q - lfbuf);
            if (kWriteFile((HANDLE)crt_ioinfo(fh)->osfhnd, lfbuf, (DWORD)n, &written, 0)) {
                charcount += (int32_t)written;
                if ((int32_t)written < n) break;
            } else {
                err = kGetLastError();
                break;
            }
        }
    } else {
        if (kWriteFile((HANDLE)crt_ioinfo(fh)->osfhnd, buf, cnt, &written, 0)) {
            err = 0;
            charcount = (int32_t)written;
        } else {
            err = kGetLastError();
        }
    }
    if (charcount) return charcount - lfcount;
    if (err) {
        if (err == 5) {
            crt_errno = CRT_EBADF;
            crt_doserrno = err;
        } else {
            gcall<void>(F_dosmaperr, err);
        }
        return -1;
    }
    if ((crt_osfile(fh) & CRT_FDEV) && *buf == 0x1a) return 0;
    crt_errno = CRT_ENOSPC;
    crt_doserrno = 0;
    return -1;
}
FP_IO(fp_write, int32_t, const char*, uint32_t)
PORT_FN(0x004d59e0, "write", c_write, fp_write)

// _close (0x4d58d0)
static int32_t __cdecl c_close(int32_t fh) {
    if (fh_bad(fh)) {
        crt_errno = CRT_EBADF;
        crt_doserrno = 0;
        return -1;
    }
    DWORD err;
    if ((fh == 1 || fh == 2) && osfhandle(1) == osfhandle(2)) err = 0;
    else err = kCloseHandle((HANDLE)osfhandle(fh)) ? 0 : kGetLastError();
    gcall<int32_t>(F_free_osfhnd, fh);
    if (err) {
        gcall<void>(F_dosmaperr, err);
        return -1;
    }
    crt_osfile(fh) = 0;
    return 0;
}
FP_IO(fp_close, int32_t)
PORT_FN(0x004d58d0, "close", c_close, fp_close)

// _commit (0x4d74a0)
static int32_t __cdecl c_commit(int32_t fh) {
    if (fh_bad(fh)) {
        crt_errno = CRT_EBADF;
        return -1;
    }
    const DWORD r = kFlushFileBuffers((HANDLE)osfhandle(fh)) ? 0 : kGetLastError();
    if (!r) return 0;
    crt_errno = CRT_EBADF;
    crt_doserrno = r;
    return -1;
}
FP_IO(fp_commit, int32_t)
PORT_FN(0x004d74a0, "commit", c_commit, fp_commit)

// _chsize (0x4d8210): extend with zeros (in binary mode) or cut with SetEndOfFile; the position restored
static int32_t __cdecl c_chsize(int32_t fh, int32_t size) {
    int32_t retval = 0;
    if (fh_bad(fh)) {
        crt_errno = CRT_EBADF;
        return -1;
    }
    const int32_t place = gcall<int32_t>(F_lseek, fh, 0, 1u);
    if (place == -1) return -1;
    const int32_t filend = gcall<int32_t>(F_lseek, fh, 0, 2u);
    if (filend == -1) return -1;
    int32_t extend = size - filend;
    if (extend > 0) {
        char blanks[0x1000];
        memset(blanks, 0, sizeof blanks);
        const int32_t oldmode = gcall<int32_t>(F_setmode, fh, 0x8000);
        do {
            int32_t n = extend >= 0x1000 ? 0x1000 : extend;
            n = gcall<int32_t>(F_write, fh, (const char*)blanks, (uint32_t)n);
            if (n == -1) {
                if (crt_doserrno == 5) crt_errno = CRT_EACCES;
                retval = n;
                break;
            }
            extend -= n;
        } while (extend > 0);
        gcall<int32_t>(F_setmode, fh, oldmode);
    } else if (extend < 0) {
        gcall<int32_t>(F_lseek, fh, size, 0u);
        retval = kSetEndOfFile((HANDLE)osfhandle(fh)) ? 0 : -1;
        if (retval == -1) {
            crt_errno = CRT_EACCES;
            crt_doserrno = kGetLastError();
        }
    }
    gcall<int32_t>(F_lseek, fh, place, 0u);
    return retval;
}
FP_IO(fp_chsize, int32_t, int32_t)
PORT_FN(0x004d8210, "chsize", c_chsize, fp_chsize)

// _sopen (0x4d7510)
static int32_t __cdecl c_sopen(const char* path, uint32_t oflag, int32_t shflag, uint32_t pmode) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = 0xc;
    sa.lpSecurityDescriptor = 0;
    sa.bInheritHandle = 0;
    if (!(oflag & 0x80)) sa.bInheritHandle = 1;                // _O_NOINHERIT
    uint8_t ff;
    if (oflag & 0x8000) ff = 0;                                 // _O_BINARY
    else if (oflag & 0x4000) ff = CRT_FTEXT;                    // _O_TEXT
    else ff = G32(CRT_FMODE_VA) != 0x8000 ? CRT_FTEXT : 0;
    DWORD access, share, create;
    switch (oflag & 3) {
    case 0: access = GENERIC_READ; break;
    case 1: access = GENERIC_WRITE; break;
    case 2: access = GENERIC_READ | GENERIC_WRITE; break;
    default:
        crt_errno = CRT_EINVAL;
        crt_doserrno = 0;
        return -1;
    }
    switch (shflag) {
    case 0x10: share = 0; break;                                // _SH_DENYRW
    case 0x20: share = FILE_SHARE_READ; break;                  // _SH_DENYWR
    case 0x30: share = FILE_SHARE_WRITE; break;                 // _SH_DENYRD
    case 0x40: share = FILE_SHARE_READ | FILE_SHARE_WRITE; break;   // _SH_DENYNO
    default:
        crt_errno = CRT_EINVAL;
        crt_doserrno = 0;
        return -1;
    }
    switch (oflag & 0x700) {                                    // _O_CREAT 0x100, _O_TRUNC 0x200, _O_EXCL 0x400
    case 0x000: case 0x400: create = OPEN_EXISTING; break;
    case 0x100: create = OPEN_ALWAYS; break;
    case 0x200: case 0x600: create = TRUNCATE_EXISTING; break;
    case 0x300: create = CREATE_ALWAYS; break;
    default: create = CREATE_NEW; break;                         // 0x500, 0x700
    }
    DWORD attr = FILE_ATTRIBUTE_NORMAL;
    if ((oflag & 0x100) && !(~G32(CRT_UMASKVAL_VA) & pmode & 0x80)) attr = FILE_ATTRIBUTE_READONLY;
    if (oflag & 0x40) {                                         // _O_TEMPORARY
        access |= DELETE;
        attr |= FILE_FLAG_DELETE_ON_CLOSE;
    }
    if (oflag & 0x1000) attr |= FILE_ATTRIBUTE_TEMPORARY;       // _O_SHORT_LIVED
    if (oflag & 0x20) attr |= FILE_FLAG_SEQUENTIAL_SCAN;        // _O_SEQUENTIAL
    else if (oflag & 0x10) attr |= FILE_FLAG_RANDOM_ACCESS;     // _O_RANDOM
    const int32_t fh = gcall<int32_t>(F_alloc_osfhnd);
    if (fh == -1) {
        crt_errno = CRT_EMFILE;
        crt_doserrno = 0;
        return -1;
    }
    const HANDLE h = kCreateFileA(path, access, share, &sa, create, attr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        gcall<void>(F_dosmaperr, kGetLastError());
        return -1;
    }
    const DWORD type = kGetFileType(h);
    if (!type) {
        kCloseHandle(h);
        gcall<void>(F_dosmaperr, kGetLastError());
        return -1;
    }
    if (type == FILE_TYPE_CHAR) ff |= CRT_FDEV;
    else if (type == FILE_TYPE_PIPE) ff |= CRT_FPIPE;
    ff |= CRT_FOPEN;
    gcall<int32_t>(F_set_osfhnd, fh, (intptr_t)h);
    crt_osfile(fh) = ff;
    const uint8_t devpipe = ff & (CRT_FDEV | CRT_FPIPE);
    if (!devpipe && (ff & CRT_FTEXT) && (oflag & 2)) {          // _O_RDWR text: chop a final ^Z
        const int32_t pos = gcall<int32_t>(F_lseek, fh, -1, 2u);
        if (pos == -1) {
            if (crt_doserrno != 0x83) {
                gcall<int32_t>(F_close, fh);
                return -1;
            }
        } else {
            char ch = 0;
            if (gcall<int32_t>(F_read, fh, &ch, 1u) == 0 && ch == 0x1a &&
                gcall<int32_t>(F_chsize, fh, pos) == -1) {
                gcall<int32_t>(F_close, fh);
                return -1;
            }
            if (gcall<int32_t>(F_lseek, fh, 0, 0u) == -1) {
                gcall<int32_t>(F_close, fh);
                return -1;
            }
        }
    }
    if (!devpipe && (oflag & 8)) crt_osfile(fh) |= CRT_FAPPEND;   // _O_APPEND
    return fh;
}
FP_IO(fp_sopen, const char*, uint32_t, int32_t, uint32_t)
PORT_FN(0x004d7510, "sopen", c_sopen, fp_sopen)

// =====================================================================================================================
// streams
// =====================================================================================================================
// _getbuf (0x4d6630): a 4096-byte buffer, or the stream's own 2-byte one (_IONBF)
static void __cdecl c_getbuf(CrtFile* s) {
    crt_cflush++;
    s->base = gcall<char*>(F_malloc, 0x1000u);
    if (s->base) {
        s->flag |= CRT_IOMYBUF;
        s->bufsiz = 0x1000;
    } else {
        s->flag |= CRT_IONBF;
        s->base = (char*)&s->charbuf;
        s->bufsiz = 2;
    }
    s->ptr = s->base;
    s->cnt = 0;
}
FP_IO(fp_getbuf, CrtFile*)
PORT_FN(0x004d6630, "getbuf", c_getbuf, fp_getbuf)

// _freebuf (0x4d59a0)
static void __cdecl c_freebuf(CrtFile* s) {
    const uint32_t fl = s->flag;
    if ((fl & 0x83) && (fl & CRT_IOMYBUF)) {
        gcall<void>(F_free, s->base);
        s->ptr = 0;
        s->flag &= ~(uint32_t)(CRT_IOMYBUF | CRT_IOSETVBUF);
        s->base = 0;
        s->cnt = 0;
    }
}
FP_IO(fp_freebuf, CrtFile*)
PORT_FN(0x004d59a0, "freebuf", c_freebuf, fp_freebuf)

// _flush (0x4d51d0): write out what a write stream holds
static int32_t __cdecl c_flush(CrtFile* s) {
    int32_t rc = 0;
    const uint32_t fl = s->flag;
    if ((fl & 3) == CRT_IOWRT && (fl & 0x108)) {
        const int32_t n = (int32_t)(s->ptr - s->base);
        if (n > 0) {
            if (gcall<int32_t>(F_write, s->file, (const char*)s->base, (uint32_t)n) == n) {
                const uint32_t f2 = s->flag;
                if (f2 & CRT_IORW) s->flag = f2 & ~(uint32_t)CRT_IOWRT;
            } else {
                s->flag |= CRT_IOERR;
                rc = -1;
            }
        }
    }
    s->ptr = s->base;
    s->cnt = 0;
    return rc;
}
FP_IO(fp_flush, CrtFile*)
PORT_FN(0x004d51d0, "flush", c_flush, fp_flush)

// fflush (0x4d5180): 0 flushes every write stream (flsall(0)); _IOCOMMIT also commits
static int32_t __cdecl c_fflush(CrtFile* s) {
    if (!s) return gcall<int32_t>(F_flsall, 0);
    if (gcall<int32_t>(F_flush, s)) return -1;
    if (s->flag & CRT_IOCOMMIT) return gcall<int32_t>(F_commit, s->file) ? -1 : 0;
    return 0;
}
FP_IO(fp_fflush, CrtFile*)
PORT_FN(0x004d5180, "fflush", c_fflush, fp_fflush)

// flsall (0x4d5250): 1 = _flushall (counts), 0 = fflush(NULL) (write streams; EOF if any failed). 512 entries.
static int32_t __cdecl c_flsall(int32_t flushflag) {
    int32_t count = 0, err = 0;
    for (uint32_t i = 0; i < 0x800; i += 4) {
        CrtFile* s = *(CrtFile**)((uint8_t*)crt_piob + i);
        if (!s) continue;
        const uint32_t fl = s->flag;
        if (!(fl & 0x83)) continue;
        if (flushflag == 1) {
            if (gcall<int32_t>(F_fflush, s) != -1) count++;
        } else if (flushflag == 0 && (fl & CRT_IOWRT)) {
            if (gcall<int32_t>(F_fflush, s) == -1) err = -1;
        }
    }
    return flushflag == 1 ? count : err;
}
FP_IO(fp_flsall, int32_t)
PORT_FN(0x004d5250, "flsall", c_flsall, fp_flsall)

// _flushall (0x4d5240)
static int32_t __cdecl c_flushall() { return gcall<int32_t>(F_flsall, 1); }
static void fp_flushall(Footprint& f) { fp_io(f); }
PORT_FN(0x004d5240, "flushall", c_flushall, fp_flushall)

// _flsbuf (0x4d2160): the buffer out, then ch into it (or written alone, unbuffered)
static int32_t __cdecl c_flsbuf(int32_t ch, CrtFile* s) {
    const int32_t fh = s->file;
    uint32_t fl = s->flag;
    if (!(fl & (CRT_IOWRT | CRT_IORW)) || (fl & CRT_IOSTRG)) {
        s->flag = fl | CRT_IOERR;
        return -1;
    }
    if (fl & CRT_IOREAD) {
        s->cnt = 0;
        fl = s->flag;
        if (!(fl & CRT_IOEOF)) {
            s->flag = fl | CRT_IOERR;
            return -1;
        }
        s->ptr = s->base;
        s->flag &= ~(uint32_t)CRT_IOREAD;
    }
    fl = s->flag | CRT_IOWRT;
    s->flag = fl;
    s->flag = fl & ~(uint32_t)CRT_IOEOF;
    s->cnt = 0;
    int32_t written = 0, charcount;
    if (!(s->flag & 0x10c)) {
        if (!(((uint32_t)(uintptr_t)s == CRT_STDOUT_VA || (uint32_t)(uintptr_t)s == CRT_STDERR_VA) &&
              gcall<int32_t>(F_isatty, fh)))
            gcall<void>(F_getbuf, s);
    }
    if (s->flag & 0x108) {
        char* b = s->base;
        charcount = (int32_t)(s->ptr - b);
        s->ptr = b + 1;
        s->cnt = s->bufsiz - 1;
        if (charcount > 0) written = gcall<int32_t>(F_write, fh, (const char*)s->base, (uint32_t)charcount);
        else if (io_safe(fh)->osfile & CRT_FAPPEND) gcall<int32_t>(F_lseek, fh, 0, 2u);
        *s->base = (char)ch;
    } else {
        charcount = 1;
        written = gcall<int32_t>(F_write, fh, (const char*)&ch, 1u);
    }
    if (written != charcount) {
        s->flag |= CRT_IOERR;
        return -1;
    }
    return ch & 0xff;
}
FP_IO(fp_flsbuf, int32_t, CrtFile*)
PORT_FN(0x004d2160, "flsbuf", c_flsbuf, fp_flsbuf)

// _filbuf (0x4d52c0): refill, and the first character
static int32_t __cdecl c_filbuf(CrtFile* s) {
    uint32_t fl = s->flag;
    if (!(fl & 0x83) || (fl & CRT_IOSTRG)) return -1;
    if (fl & CRT_IOWRT) {
        s->flag = fl | CRT_IOERR;
        return -1;
    }
    fl |= CRT_IOREAD;
    s->flag = fl;
    if (!(fl & 0x10c)) gcall<void>(F_getbuf, s);
    else s->ptr = s->base;
    const int32_t n = gcall<int32_t>(F_read, s->file, s->base, (uint32_t)s->bufsiz);
    s->cnt = n;
    if (n == 0 || n == -1) {
        s->flag |= n ? CRT_IOERR : CRT_IOEOF;
        s->cnt = 0;
        return -1;
    }
    const uint32_t f2 = s->flag;
    if (!(f2 & (CRT_IOWRT | CRT_IORW)) && (io_safe(s->file)->osfile & 0x82) == 0x82) s->flag = f2 | CRT_IOCTRLZ;
    if (s->bufsiz == 0x200) {
        const uint32_t f3 = s->flag;
        if ((f3 & CRT_IOMYBUF) && !(f3 & CRT_IOSETVBUF)) s->bufsiz = 0x1000;
    }
    s->cnt--;
    const uint8_t* p = (const uint8_t*)s->ptr;
    s->ptr = (char*)p + 1;
    return *p;
}
FP_IO(fp_filbuf, CrtFile*)
PORT_FN(0x004d52c0, "filbuf", c_filbuf, fp_filbuf)

// ungetc (0x4d6450)
static int32_t __cdecl c_ungetc(int32_t ch, CrtFile* s) {
    if (ch == -1) return -1;
    const uint32_t fl = s->flag;
    if (!(fl & CRT_IOREAD) && !((fl & CRT_IORW) && !(fl & CRT_IOWRT))) return -1;
    if (!s->base) gcall<void>(F_getbuf, s);
    if (s->ptr == s->base) {
        if (s->cnt) return -1;
        s->ptr++;
    }
    if (s->flag & CRT_IOSTRG) {
        char* p = --s->ptr;
        if (*p != (char)ch) {
            s->ptr = p + 1;
            return -1;
        }
    } else {
        char* p = --s->ptr;
        *p = (char)ch;
    }
    s->cnt++;
    const uint32_t f2 = s->flag & ~(uint32_t)CRT_IOEOF;
    s->flag = f2;
    s->flag = f2 | CRT_IOREAD;
    return ch & 0xff;
}
static void fp_ungetc(Footprint& f, int32_t, CrtFile* s) {
    f.add(s, sizeof *s, "FILE");
    if (!s->base) {
        f.replay_only = "allocates the stream's buffer (_getbuf)";
        return;
    }
    f.add(s->ptr == s->base ? s->base : s->ptr - 1, 1, "the character put back");
}
PORT_FN(0x004d6450, "ungetc", c_ungetc, fp_ungetc)

// _getstream (0x4d5830): a free FILE (a new one allocated in the first empty __piob slot)
static CrtFile* __cdecl c_getstream() {
    CrtFile* r = 0;
    if (crt_nstream > 0) {
        CrtFile** e = crt_piob;
        for (int32_t i = 0;; i++, e++) {
            CrtFile* s = *e;
            if (!s) {
                crt_piob[i] = gcall<CrtFile*>(F_malloc, 0x20u);
                if (crt_piob[i]) r = crt_piob[i];
                break;
            }
            if (!(s->flag & 0x83)) {
                r = crt_piob[i];
                break;
            }
            if (crt_nstream <= i + 1) break;
        }
    }
    if (r) {
        r->cnt = 0;
        r->flag = 0;
        r->base = 0;
        r->ptr = 0;
        r->tmpfname = 0;
        r->file = -1;
    }
    return r;
}
static void fp_getstream(Footprint& f) { fp_io(f); }
PORT_FN(0x004d5830, "getstream", c_getstream, fp_getstream)

// _openfile (0x4d5620): the mode string, then _sopen (pmode 0644)
static CrtFile* __cdecl c_openfile(const char* file, const char* mode, int32_t shflag, CrtFile* stream) {
    uint32_t modeflag, streamflag;
    switch (*mode) {
    case 'a': modeflag = 0x109; streamflag = G32(CRT_COMMODE_VA) | CRT_IOWRT; break;
    case 'r': modeflag = 0; streamflag = G32(CRT_COMMODE_VA) | CRT_IOREAD; break;
    case 'w': modeflag = 0x301; streamflag = G32(CRT_COMMODE_VA) | CRT_IOWRT; break;
    default: return 0;
    }
    int32_t valid = 1, commodeset = 0, scanset = 0;
    for (mode++; *mode && valid; mode++) {
        switch (*mode) {
        case '+':
            if (modeflag & 2) valid = 0;
            else {
                modeflag = (modeflag | 2) & ~1u;
                streamflag = (streamflag | CRT_IORW) & ~3u;
            }
            break;
        case 'D':
            if (modeflag & 0x40) valid = 0;
            else modeflag |= 0x40;
            break;
        case 'R':
            if (scanset) valid = 0;
            else {
                scanset = 1;
                modeflag |= 0x10;
            }
            break;
        case 'S':
            if (scanset) valid = 0;
            else {
                scanset = 1;
                modeflag |= 0x20;
            }
            break;
        case 'T':
            if (modeflag & 0x1000) valid = 0;
            else modeflag |= 0x1000;
            break;
        case 'b':
            if (modeflag & 0xc000) valid = 0;
            else modeflag |= 0x8000;
            break;
        case 'c':
            if (commodeset) valid = 0;
            else {
                commodeset = 1;
                streamflag |= CRT_IOCOMMIT;
            }
            break;
        case 'n':
            if (commodeset) valid = 0;
            else {
                commodeset = 1;
                streamflag &= ~(uint32_t)CRT_IOCOMMIT;
            }
            break;
        case 't':
            if (modeflag & 0xc000) valid = 0;
            else modeflag |= 0x4000;
            break;
        default: valid = 0; break;
        }
    }
    const int32_t fh = gcall<int32_t>(F_sopen, file, modeflag, shflag, 0x1a4u);
    if (fh < 0) return 0;
    crt_cflush++;
    stream->flag = (int32_t)streamflag;
    stream->cnt = 0;
    stream->ptr = 0;
    stream->base = 0;
    stream->tmpfname = 0;
    stream->file = fh;
    return stream;
}
FP_IO(fp_openfile, const char*, const char*, int32_t, CrtFile*)
PORT_FN(0x004d5620, "openfile", c_openfile, fp_openfile)

// _fsopen (0x4d05e0) / fopen (0x4d0610, _SH_DENYNO)
static CrtFile* __cdecl c_fsopen(const char* file, const char* mode, int32_t shflag) {
    CrtFile* s = gcall<CrtFile*>(F_getstream);
    if (!s) return 0;
    return gcall<CrtFile*>(F_openfile, file, mode, shflag, s);
}
FP_IO(fp_fsopen, const char*, const char*, int32_t)
PORT_FN(0x004d05e0, "fsopen", c_fsopen, fp_fsopen)
static CrtFile* __cdecl c_fopen(const char* file, const char* mode) { return gcall<CrtFile*>(F_fsopen, file, mode, 0x40); }
FP_IO(fp_fopen, const char*, const char*)
PORT_FN(0x004d0610, "fopen", c_fopen, fp_fopen)

// fclose (0x4d0630)
static int32_t __cdecl c_fclose(CrtFile* s) {
    int32_t result = -1;
    const uint32_t fl = s->flag;
    if (fl & CRT_IOSTRG) {
        s->flag = 0;
        return -1;
    }
    if (fl & 0x83) {
        result = gcall<int32_t>(F_flush, s);
        gcall<void>(F_freebuf, s);
        if (gcall<int32_t>(F_close, s->file) < 0) {
            result = -1;
        } else if (s->tmpfname) {
            gcall<void>(F_free, s->tmpfname);
            s->tmpfname = 0;
        }
    }
    s->flag = 0;
    return result;
}
FP_IO(fp_fclose, CrtFile*)
PORT_FN(0x004d0630, "fclose", c_fclose, fp_fclose)

// fread (0x4d0490)
static uint32_t __cdecl c_fread(void* buffer, uint32_t size, uint32_t num, CrtFile* s) {
    char* data = (char*)buffer;
    const uint32_t total = size * num;
    uint32_t count = total;
    if (!count) return 0;
    uint32_t bufsize = (s->flag & 0x10c) ? (uint32_t)s->bufsiz : 0x1000;
    while (count) {
        if ((s->flag & 0x10c) && s->cnt) {
            const uint32_t nb = count < (uint32_t)s->cnt ? count : (uint32_t)s->cnt;
            rep_movs(data, s->ptr, nb);
            count -= nb;
            s->cnt -= (int32_t)nb;
            s->ptr += nb;
            data += nb;
        } else if (count >= bufsize) {
            const uint32_t nb = bufsize ? count - count % bufsize : count;
            const int32_t nr = gcall<int32_t>(F_read, s->file, data, nb);
            if (nr == 0) {
                s->flag |= CRT_IOEOF;
                return (total - count) / size;
            }
            if (nr == -1) {
                s->flag |= CRT_IOERR;
                return (total - count) / size;
            }
            count -= (uint32_t)nr;
            data += nr;
        } else {
            const int32_t c = gcall<int32_t>(F_filbuf, s);
            if (c == -1) return (total - count) / size;
            *data++ = (char)c;
            count--;
            bufsize = (uint32_t)s->bufsiz;
        }
    }
    return num;
}
FP_IO(fp_fread, void*, uint32_t, uint32_t, CrtFile*)
PORT_FN(0x004d0490, "fread", c_fread, fp_fread)

// fwrite (0x4d06a0)
static uint32_t __cdecl c_fwrite(const void* buffer, uint32_t size, uint32_t num, CrtFile* s) {
    const char* data = (const char*)buffer;
    const uint32_t total = size * num;
    uint32_t count = total;
    if (!count) return 0;
    uint32_t bufsize = (s->flag & 0x10c) ? (uint32_t)s->bufsiz : 0x1000;
    while (count) {
        const uint32_t big = s->flag & 0x108;
        if (big && s->cnt) {
            const uint32_t nb = (uint32_t)s->cnt < count ? (uint32_t)s->cnt : count;
            rep_movs(s->ptr, data, nb);
            count -= nb;
            s->cnt -= (int32_t)nb;
            s->ptr += nb;
            data += nb;
        } else if (count >= bufsize) {
            if (big && gcall<int32_t>(F_flush, s)) return (total - count) / size;
            const uint32_t nb = bufsize ? count - count % bufsize : count;
            const int32_t nw = gcall<int32_t>(F_write, s->file, data, nb);
            if (nw == -1) {
                s->flag |= CRT_IOERR;
                return (total - count) / size;
            }
            count -= (uint32_t)nw;
            data += nw;
            if ((uint32_t)nw < nb) {
                s->flag |= CRT_IOERR;
                return (total - count) / size;
            }
        } else {
            if (gcall<int32_t>(F_flsbuf, (int32_t)(int8_t)*data, s) == -1) return (total - count) / size;
            data++;
            count--;
            bufsize = (uint32_t)s->bufsiz;
            if ((int32_t)bufsize <= 0) bufsize = 1;
        }
    }
    return num;
}
FP_IO(fp_fwrite, const void*, uint32_t, uint32_t, CrtFile*)
PORT_FN(0x004d06a0, "fwrite", c_fwrite, fp_fwrite)

// ftell (0x4d02e0)
static int32_t __cdecl c_ftell(CrtFile* s) {
    const int32_t fd = s->file;
    if (s->cnt < 0) s->cnt = 0;
    int32_t filepos = gcall<int32_t>(F_lseek, fd, 0, 1u);
    if (filepos < 0) return -1;
    const uint32_t fl = s->flag;
    if (!(fl & 0x108)) return filepos - s->cnt;
    const char* ptr = s->ptr;
    const char* base = s->base;
    const uint32_t offset0 = (uint32_t)(ptr - base);
    uint32_t offset = offset0;
    if (fl & 3) {
        if (crt_osfile(fd) & CRT_FTEXT)
            for (const char* p = base; p < ptr; p++)
                if (*p == '\n') offset++;
    } else if (!(fl & CRT_IORW)) {
        crt_errno = CRT_EINVAL;
        return -1;
    }
    if (filepos == 0) return (int32_t)offset;
    if (fl & CRT_IOREAD) {
        if (!s->cnt) return filepos;
        uint32_t rdcnt = offset0 + (uint32_t)s->cnt;
        if (crt_osfile(fd) & CRT_FTEXT) {
            if (gcall<int32_t>(F_lseek, fd, 0, 2u) == filepos) {
                const char* b = s->base;
                const char* mx = b + rdcnt;
                for (const char* p = b; p < mx; p++)
                    if (*p == '\n') rdcnt++;
                if (s->flag & CRT_IOCTRLZ) rdcnt++;
            } else {
                gcall<int32_t>(F_lseek, fd, filepos, 0u);
                uint32_t f2;
                if (rdcnt <= 0x200 && ((f2 = s->flag) & CRT_IOMYBUF) && !(f2 & CRT_IOSETVBUF)) rdcnt = 0x200;
                else rdcnt = (uint32_t)s->bufsiz;
                if (crt_osfile(fd) & CRT_FCRLF) rdcnt++;
            }
        }
        filepos -= (int32_t)rdcnt;
    }
    return filepos + (int32_t)offset;
}
FP_IO(fp_ftell, CrtFile*)
PORT_FN(0x004d02e0, "ftell", c_ftell, fp_ftell)

// fseek (0x4d0200)
static int32_t __cdecl c_fseek(CrtFile* s, int32_t offset, int32_t whence) {
    const uint32_t fl = s->flag;
    if (!(fl & 0x83) || (whence != 0 && whence != 1 && whence != 2)) {
        crt_errno = CRT_EINVAL;
        return -1;
    }
    s->flag = fl & ~(uint32_t)CRT_IOEOF;
    if (whence == 1) {
        offset += gcall<int32_t>(F_ftell, s);
        whence = 0;
    }
    gcall<int32_t>(F_flush, s);
    const uint32_t f2 = s->flag;
    if (f2 & CRT_IORW) s->flag = f2 & ~3u;
    else if ((f2 & CRT_IOREAD) && (f2 & CRT_IOMYBUF) && !(f2 & CRT_IOSETVBUF)) s->bufsiz = 0x200;
    return gcall<int32_t>(F_lseek, s->file, offset, (uint32_t)whence) == -1 ? -1 : 0;
}
FP_IO(fp_fseek, CrtFile*, int32_t, int32_t)
PORT_FN(0x004d0200, "fseek", c_fseek, fp_fseek)

// _stbuf (0x4d4ee0): stdout / stderr on a device get a temporary buffer (_stdbuf) for one printf
static int32_t __cdecl c_stbuf(CrtFile* s) {
    if (!gcall<int32_t>(F_isatty, s->file)) return 0;
    uint32_t index;
    if ((uint32_t)(uintptr_t)s == CRT_STDOUT_VA) index = 0;
    else if ((uint32_t)(uintptr_t)s == CRT_STDERR_VA) index = 1;
    else return 0;
    crt_cflush++;
    if (s->flag & 0x10c) return 0;
    char* volatile* slot = (char* volatile*)(uintptr_t)(CRT_STDBUF_VA + index * 4);
    if (!*slot) {
        *slot = gcall<char*>(F_malloc, 0x1000u);
        if (!*slot) return 0;
    }
    char* b = *slot;
    s->base = b;
    s->ptr = b;
    s->bufsiz = 0x1000;
    s->cnt = 0x1000;
    s->flag |= CRT_IOWRT | CRT_IOYOURBUF | CRT_IOFLRTN;
    return 1;
}
FP_IO(fp_stbuf, CrtFile*)
PORT_FN(0x004d4ee0, "stbuf", c_stbuf, fp_stbuf)

// _ftbuf (0x4d4f80)
static void __cdecl c_ftbuf(int32_t flag, CrtFile* s) {
    if (flag) {
        if (s->flag & CRT_IOFLRTN) {
            gcall<int32_t>(F_flush, s);
            s->flag &= ~(uint32_t)(CRT_IOYOURBUF | CRT_IOFLRTN);
            s->bufsiz = 0;
            s->ptr = 0;
            s->base = 0;
        }
    } else if (s->flag & CRT_IOFLRTN) {
        gcall<int32_t>(F_flush, s);
    }
}
FP_IO(fp_ftbuf, int32_t, CrtFile*)
PORT_FN(0x004d4f80, "ftbuf", c_ftbuf, fp_ftbuf)

// __initstdio (0x4d4fd0): __piob (512 entries, 20 if that fails), the 20 _iob entries in it, and stdin / stdout /
// stderr unhooked (_file -1) when their OS handles are invalid
static void __cdecl c_initstdio() {
    if (crt_nstream == 0) crt_nstream = 0x200;
    else if (crt_nstream < 0x14) crt_nstream = 0x14;
    crt_piob = gcall<CrtFile**>(F_calloc, (uint32_t)crt_nstream, 4u);
    if (!crt_piob) {
        crt_nstream = 0x14;
        crt_piob = gcall<CrtFile**>(F_calloc, 0x14u, 4u);
        if (!crt_piob) gcall<void>(F_amsg_exit, 0x1a);
    }
    for (uint32_t i = 0; i < 0x14; i++) crt_piob[i] = (CrtFile*)(uintptr_t)(CRT_IOB_VA + i * 0x20);
    for (int32_t i = 0; i < 3; i++) {
        const intptr_t h = crt_ioinfo(i)->osfhnd;
        if (h == -1 || h == 0) CRT_G(int32_t, CRT_IOB_VA + 0x10 + i * 0x20) = -1;
    }
}
static void fp_initstdio(Footprint& f) { f.replay_only = "allocates __piob (start-up)"; }
PORT_FN(0x004d4fd0, "initstdio", c_initstdio, fp_initstdio)

// __endstdio (0x4d50a0): _flushall, and _fcloseall when exiting (the deferred original)
static void __cdecl c_endstdio() {
    gcall<int32_t>(F_flushall);
    if (CRT_G(uint8_t, CRT_EXITFLAG_VA)) gcall<int32_t>(F_fcloseall);
}
static void fp_endstdio(Footprint& f) { f.replay_only = "flushes and closes the streams (exit)"; }
PORT_FN(0x004d50a0, "endstdio", c_endstdio, fp_endstdio)

// =====================================================================================================================
// directories: getcwd.obj, chdir.obj, fullpath.obj
// =====================================================================================================================
// _validdrive (0x4d4a90): 0 is the current drive; otherwise GetDriveType of "X:\"
static int32_t __cdecl c_validdrive(uint32_t drive) {
    if (!drive) return 1;
    char d[4];
    d[1] = ':';
    d[2] = '\\';
    d[3] = 0;
    d[0] = (char)(drive + 0x40);
    const UINT t = kGetDriveTypeA(d);
    return t == DRIVE_UNKNOWN || t == DRIVE_NO_ROOT_DIR ? 0 : 1;
}
static void fp_validdrive(Footprint& f, uint32_t) { f.replay_only = "asks Windows (GetDriveType)"; }
PORT_FN(0x004d4a90, "validdrive", c_validdrive, fp_validdrive)

// _getdcwd (0x4d4960): the drive's current directory ("X:." through GetFullPathName), or the process's (0)
static char* __cdecl c_getdcwd(int32_t drive, char* buf, int32_t maxlen) {
    char drv[4];
    char* fname;
    char dir[0x104];
    DWORD len;
    if (drive) {
        if (!gcall<int32_t>(F_validdrive, drive)) {
            crt_doserrno = 0xf;
            crt_errno = CRT_EACCES;
            return 0;
        }
        drv[0] = (char)(drive + 0x40);
        drv[1] = ':';
        drv[2] = '.';
        drv[3] = 0;
        len = kGetFullPathNameA(drv, 0x104, dir, &fname);
    } else {
        len = kGetCurrentDirectoryA(0x104, dir);
    }
    if (!len) return 0;
    len++;
    if (len > 0x104) return 0;
    char* p;
    if (!buf) {
        p = gcall<char*>(F_malloc, (uint32_t)((int32_t)len > maxlen ? (int32_t)len : maxlen));
        if (!p) {
            crt_errno = CRT_ENOMEM;
            return 0;
        }
    } else {
        if ((int32_t)len > maxlen) {
            crt_errno = CRT_ERANGE;
            return 0;
        }
        p = buf;
    }
    rep_movs(p, dir, (uint32_t)strlen(dir) + 1);
    return p;
}
FP_IO(fp_getdcwd, int32_t, char*, int32_t)
PORT_FN(0x004d4960, "getdcwd", c_getdcwd, fp_getdcwd)

// _getcwd (0x4d4940)
static char* __cdecl c_getcwd(char* buf, int32_t maxlen) { return gcall<char*>(F_getdcwd, 0, buf, maxlen); }
FP_IO(fp_getcwd, char*, int32_t)
PORT_FN(0x004d4940, "getcwd", c_getcwd, fp_getcwd)

// _chdir (0x4cf830): and the drive's "=X:" environment variable (not for a UNC path)
static int32_t __cdecl c_chdir(const char* path) {
    alignas(4) char fr[0x10c];                      // the "=X:" name at +0, the directory at +4
    if (kSetCurrentDirectoryA(path) && kGetCurrentDirectoryA(0x105, &fr[4])) {
        if ((fr[4] == '\\' || fr[4] == '/') && fr[4] == fr[5]) return 0;
        fr[0] = '=';
        const uint32_t up = gcall<uint32_t>(F_mbctoupper, (uint32_t)(uint8_t)fr[4]);
        fr[1] = (char)up;
        fr[2] = ':';
        fr[3] = 0;
        if (kSetEnvironmentVariableA(fr, &fr[4])) return 0;
    }
    gcall<void>(F_dosmaperr, kGetLastError());
    return -1;
}
static void fp_chdir(Footprint& f, const char*) { f.replay_only = "changes the current directory"; }
PORT_FN(0x004cf830, "chdir", c_chdir, fp_chdir)

// _fullpath (0x4cfbe0)
static char* __cdecl c_fullpath(char* user, const char* path, uint32_t maxlen) {
    if (!path || !*path) return gcall<char*>(F_getcwd, user, maxlen);
    char* buf;
    if (!user) {
        buf = gcall<char*>(F_malloc, 0x104u);
        if (!buf) {
            crt_errno = CRT_ENOMEM;
            return 0;
        }
        maxlen = 0x104;
    } else {
        buf = user;
    }
    char* fname;
    const DWORD n = kGetFullPathNameA(path, maxlen, buf, &fname);
    if (n >= maxlen) {
        crt_errno = CRT_ERANGE;
        return 0;
    }
    if (!n) {
        gcall<void>(F_dosmaperr, kGetLastError());
        return 0;
    }
    return buf;
}
FP_IO(fp_fullpath, char*, const char*, uint32_t)
PORT_FN(0x004cfbe0, "fullpath", c_fullpath, fp_fullpath)

}   // namespace crt_io
