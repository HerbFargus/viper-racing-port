// win32_compat.h -- the Windows types, structures, constants and macros the port uses, for the native Linux build
// (relink stage R2b). On Linux, <windows.h> (and <mmsystem.h>, <winsock.h>) is this file: tools/build_linux.sh puts
// hook/linux_inc/ on the include path, whose windows.h includes it -- so no source changes its #include, and the
// Windows builds (MSVC, mingw) never see it.
//
// Every structure is Windows' 32-bit (i386) layout, exactly: sizes and the offsets the port or the game reads are
// static_asserted at the end. Two System V i386 ABI points differ from Windows' and are handled here:
//   * a double / 64-bit integer inside a struct is 4-aligned on Linux, 8 on Windows: such members (LARGE_INTEGER,
//     ULONGLONG fields) carry VP_WIN_ALIGN8;
//   * wchar_t is 32 bits: WCHAR is a 16-bit unit (uint16_t), never wchar_t; a wide string literal is u"..." (char16_t)
//     cast where needed. Nothing here uses wchar_t.
// Handles are Windows' (void*, and STRICT's distinct struct pointers for HWND & co.), with the stand-ins' 32-bit
// values (w32_handle.h) behind them. The functions declared at the end are the ones the port's Linux build calls
// directly -- implemented by the port (vp_os.cpp / the stand-ins) or, until agents B/C/D write them, stubbed in
// hook/linux_todo.cpp.
//
// msvc_compat.h (force-included) has the compiler side: __stdcall & co., __declspec, intrinsics, MSVC CRT names.
#pragma once
#if defined(_WIN32)
#error "win32_compat.h is for the Linux build only"
#endif
#include "msvc_compat.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VP_WIN_ALIGN8 __attribute__((aligned(8)))

// ---- base types (minwindef.h, winnt.h, basetsd.h) ---------------------------------------------------------------------------
typedef unsigned long DWORD;
typedef int BOOL;
typedef int WINBOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef float FLOAT;
typedef int INT;
typedef unsigned int UINT;
typedef long LONG;
typedef unsigned long ULONG;
typedef short SHORT;
typedef unsigned short USHORT;
typedef char CHAR;
typedef unsigned char UCHAR;
typedef uint8_t BOOLEAN;
typedef uint16_t WCHAR;                       // Windows' wchar_t: 16 bits (Linux's wchar_t is 32)
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned long long DWORDLONG;
typedef long long LONG64;
typedef unsigned long long ULONG64;
typedef unsigned long long DWORD64;
typedef int INT32;
typedef unsigned int UINT32;
typedef signed char INT8;
typedef unsigned char UINT8;
typedef short INT16;
typedef unsigned short UINT16;
typedef int INT_PTR;
typedef unsigned int UINT_PTR;
typedef long LONG_PTR;
typedef unsigned long ULONG_PTR;
typedef unsigned long DWORD_PTR;
typedef unsigned long SIZE_T;
typedef long SSIZE_T;
typedef void VOID;
typedef void* PVOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef CHAR* LPSTR;
typedef CHAR* PSTR;
typedef CHAR* PCHAR;
typedef const CHAR* LPCSTR;
typedef const CHAR* PCSTR;
typedef WCHAR* LPWSTR;
typedef WCHAR* PWSTR;
typedef const WCHAR* LPCWSTR;
typedef const WCHAR* PCWSTR;
typedef BYTE* PBYTE;
typedef BYTE* LPBYTE;
typedef WORD* PWORD;
typedef WORD* LPWORD;
typedef DWORD* PDWORD;
typedef DWORD* LPDWORD;
typedef LONG* PLONG;
typedef LONG* LPLONG;
typedef ULONG* PULONG;
typedef BOOL* PBOOL;
typedef BOOL* LPBOOL;
typedef INT* PINT;
typedef INT* LPINT;
typedef UINT* PUINT;
typedef SIZE_T* PSIZE_T;
typedef ULONG_PTR* PULONG_PTR;
typedef DWORD_PTR* PDWORD_PTR;
typedef WORD ATOM;
typedef DWORD COLORREF;
typedef DWORD LCID;
typedef WORD LANGID;
typedef DWORD LCTYPE;
typedef LONG HRESULT;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef DWORD ACCESS_MASK;

typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef HANDLE* LPHANDLE;
#define DECLARE_HANDLE(name) struct name##__ { int unused; }; typedef struct name##__* name
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HPALETTE);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HPEN);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HHOOK);
DECLARE_HANDLE(HGLRC);
DECLARE_HANDLE(HMONITOR);
DECLARE_HANDLE(HRSRC);
typedef HINSTANCE HMODULE;
typedef HICON HCURSOR;
typedef HANDLE HGDIOBJ;
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
typedef HKEY* PHKEY;
typedef int(__stdcall* FARPROC)();
typedef int(__stdcall* NEARPROC)();
typedef int(__stdcall* PROC)();

#define WINAPI __stdcall
#define WINAPIV __cdecl
#define APIENTRY WINAPI
#define CALLBACK __stdcall
#define STDMETHODCALLTYPE __stdcall
#define STDAPICALLTYPE __stdcall
#define NTAPI __stdcall
#define FAR
#define NEAR
#ifndef CONST
#define CONST const
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif
#define MAX_PATH 260
// (no min / max macros: C++, as NOMINMAX -- libstdc++ can't live with them)
#define MAKEWORD(a, b) ((WORD)(((BYTE)((DWORD_PTR)(a) & 0xff)) | ((WORD)((BYTE)((DWORD_PTR)(b) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)((DWORD_PTR)(a) & 0xffff)) | ((DWORD)((WORD)((DWORD_PTR)(b) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define LOBYTE(w) ((BYTE)((DWORD_PTR)(w) & 0xff))
#define HIBYTE(w) ((BYTE)((DWORD_PTR)(w) >> 8))
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define IS_INTRESOURCE(r) ((((ULONG_PTR)(r)) >> 16) == 0)
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define UNREFERENCED_PARAMETER(p) (void)(p)

// ---- HRESULTs, errors (winerror.h) --------------------------------------------------------------------------------------------
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define MAKE_HRESULT(sev, fac, code) ((HRESULT)(((unsigned long)(sev) << 31) | ((unsigned long)(fac) << 16) | ((unsigned long)(code))))
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_ABORT ((HRESULT)0x80004004L)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFL)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define CLASS_E_NOAGGREGATION ((HRESULT)0x80040110L)

#define NO_ERROR 0L
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_INVALID_DATA 13L
#define ERROR_OUTOFMEMORY 14L
#define ERROR_INVALID_DRIVE 15L
#define ERROR_NO_MORE_FILES 18L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_LOCK_VIOLATION 33L
#define ERROR_HANDLE_EOF 38L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BROKEN_PIPE 109L
#define ERROR_CALL_NOT_IMPLEMENTED 120L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_NEGATIVE_SEEK 131L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_BAD_PATHNAME 161L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_ENVVAR_NOT_FOUND 203L
#define ERROR_MORE_DATA 234L
#define ERROR_NO_MORE_ITEMS 259L
#define ERROR_DIRECTORY 267L
#define ERROR_TOO_MANY_POSTS 298L
#define ERROR_NOACCESS 998L
#define ERROR_INVALID_WINDOW_HANDLE 1400L
#define ERROR_CLASS_ALREADY_EXISTS 1410L
#define ERROR_TIMEOUT 1460L
#define WAIT_TIMEOUT 258L

// ---- waits, processes, threads (winbase.h, minwinbase.h, winnt.h) -------------------------------------------------------------
#define INFINITE 0xFFFFFFFF
#define STATUS_WAIT_0 ((DWORD)0x00000000L)
#define STATUS_ABANDONED_WAIT_0 ((DWORD)0x00000080L)
#define WAIT_OBJECT_0 ((STATUS_WAIT_0) + 0)
#define WAIT_ABANDONED ((STATUS_ABANDONED_WAIT_0) + 0)
#define WAIT_ABANDONED_0 ((STATUS_ABANDONED_WAIT_0) + 0)
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define STILL_ACTIVE ((DWORD)0x00000103L)
#define CREATE_SUSPENDED 0x00000004
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_IDLE (-15)
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002
#define STARTF_USESHOWWINDOW 0x00000001
#define PROCESS_DEP_ENABLE 0x00000001
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
typedef DWORD(__stdcall* PTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);
typedef PTHREAD_START_ROUTINE LPTHREAD_START_ROUTINE;

// ---- memory (winnt.h, memoryapi.h, heapapi.h, winbase.h) ------------------------------------------------------------------------
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_MAPPED 0x40000
#define MEM_RESET 0x80000
#define MEM_TOP_DOWN 0x100000
#define MEM_IMAGE 0x1000000
#define SEC_IMAGE 0x1000000
#define HEAP_NO_SERIALIZE 0x00000001
#define HEAP_GROWABLE 0x00000002
#define HEAP_GENERATE_EXCEPTIONS 0x00000004
#define HEAP_ZERO_MEMORY 0x00000008
#define HEAP_REALLOC_IN_PLACE_ONLY 0x00000010
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GMEM_DDESHARE 0x2000
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR (GMEM_FIXED | GMEM_ZEROINIT)
#define FILE_MAP_COPY 0x0001
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0xF001F
#define FILE_MAP_EXECUTE 0x0020

typedef struct _MEMORY_BASIC_INFORMATION {
    PVOID BaseAddress;
    PVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

typedef struct _SYSTEM_INFO {
    union {
        DWORD dwOemId;
        struct {
            WORD wProcessorArchitecture;
            WORD wReserved;
        };
    };
    DWORD dwPageSize;
    LPVOID lpMinimumApplicationAddress;
    LPVOID lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD dwNumberOfProcessors;
    DWORD dwProcessorType;
    DWORD dwAllocationGranularity;
    WORD wProcessorLevel;
    WORD wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
#define PROCESSOR_INTEL_PENTIUM 586
#define PROCESSOR_ARCHITECTURE_INTEL 0

// ---- 64-bit values ------------------------------------------------------------------------------------------------------------
typedef union VP_WIN_ALIGN8 _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    struct {
        DWORD LowPart;
        LONG HighPart;
    } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef union VP_WIN_ALIGN8 _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;
typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
} SYSTEMTIME, *PSYSTEMTIME, *LPSYSTEMTIME;

// ---- synchronisation (the stand-ins' CRITICAL_SECTION: Windows' 0x18 bytes, w32_kernel.h's W32CriticalSection) ----------------
typedef struct _RTL_CRITICAL_SECTION_DEBUG* PRTL_CRITICAL_SECTION_DEBUG;
typedef struct _RTL_CRITICAL_SECTION {
    PRTL_CRITICAL_SECTION_DEBUG DebugInfo;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;
typedef RTL_CRITICAL_SECTION CRITICAL_SECTION;
typedef PRTL_CRITICAL_SECTION PCRITICAL_SECTION;
typedef PRTL_CRITICAL_SECTION LPCRITICAL_SECTION;

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *PPROCESS_INFORMATION, *LPPROCESS_INFORMATION;

// ---- files (winnt.h, fileapi.h, winbase.h) ---------------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define GENERIC_EXECUTE 0x20000000
#define GENERIC_ALL 0x10000000
#define DELETE 0x00010000L
#define SYNCHRONIZE 0x00100000L
#define FILE_READ_DATA 0x0001
#define FILE_WRITE_DATA 0x0002
#define FILE_APPEND_DATA 0x0004
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_NO_BUFFERING 0x20000000
#define FILE_FLAG_RANDOM_ACCESS 0x10000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK 0x0001
#define FILE_TYPE_CHAR 0x0002
#define FILE_TYPE_PIPE 0x0003
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define DRIVE_CDROM 5
#define DRIVE_RAMDISK 6

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    CHAR cFileName[MAX_PATH];
    CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA WIN32_FIND_DATA;
typedef struct _OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

// ---- the registry (winreg.h): the keys' values only ------------------------------------------------------------------------
#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)((LONG)0x80000000))
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
#define HKEY_USERS ((HKEY)(ULONG_PTR)((LONG)0x80000003))
#define HKEY_PERFORMANCE_DATA ((HKEY)(ULONG_PTR)((LONG)0x80000004))
#define HKEY_CURRENT_CONFIG ((HKEY)(ULONG_PTR)((LONG)0x80000005))
#define HKEY_DYN_DATA ((HKEY)(ULONG_PTR)((LONG)0x80000006))

// ---- code pages (winnls.h) ---------------------------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_OEMCP 1
#define CP_MACCP 2
#define CP_THREAD_ACP 3
#define CP_UTF7 65000
#define CP_UTF8 65001
#define MB_PRECOMPOSED 0x00000001
#define MB_ERR_INVALID_CHARS 0x00000008
#define WC_COMPOSITECHECK 0x00000200
#define WC_DEFAULTCHAR 0x00000040
#define MAX_LEADBYTES 12
#define MAX_DEFAULTCHAR 2
typedef struct _cpinfo {
    UINT MaxCharSize;
    BYTE DefaultChar[MAX_DEFAULTCHAR];
    BYTE LeadByte[MAX_LEADBYTES];
} CPINFO, *LPCPINFO;

// ---- exceptions (winnt.h, minwinbase.h, excpt.h; i386 layouts) ---------------------------------------------------------------
#define STATUS_SUCCESS ((DWORD)0x00000000L)
#define STATUS_GUARD_PAGE_VIOLATION ((DWORD)0x80000001L)
#define STATUS_DATATYPE_MISALIGNMENT ((DWORD)0x80000002L)
#define STATUS_BREAKPOINT ((DWORD)0x80000003L)
#define STATUS_SINGLE_STEP ((DWORD)0x80000004L)
#define STATUS_ACCESS_VIOLATION ((DWORD)0xC0000005L)
#define STATUS_IN_PAGE_ERROR ((DWORD)0xC0000006L)
#define STATUS_INVALID_HANDLE ((DWORD)0xC0000008L)
#define STATUS_NO_MEMORY ((DWORD)0xC0000017L)
#define STATUS_ILLEGAL_INSTRUCTION ((DWORD)0xC000001DL)
#define STATUS_NONCONTINUABLE_EXCEPTION ((DWORD)0xC0000025L)
#define STATUS_INVALID_DISPOSITION ((DWORD)0xC0000026L)
#define STATUS_ARRAY_BOUNDS_EXCEEDED ((DWORD)0xC000008CL)
#define STATUS_FLOAT_DENORMAL_OPERAND ((DWORD)0xC000008DL)
#define STATUS_FLOAT_DIVIDE_BY_ZERO ((DWORD)0xC000008EL)
#define STATUS_FLOAT_INEXACT_RESULT ((DWORD)0xC000008FL)
#define STATUS_FLOAT_INVALID_OPERATION ((DWORD)0xC0000090L)
#define STATUS_FLOAT_OVERFLOW ((DWORD)0xC0000091L)
#define STATUS_FLOAT_STACK_CHECK ((DWORD)0xC0000092L)
#define STATUS_FLOAT_UNDERFLOW ((DWORD)0xC0000093L)
#define STATUS_INTEGER_DIVIDE_BY_ZERO ((DWORD)0xC0000094L)
#define STATUS_INTEGER_OVERFLOW ((DWORD)0xC0000095L)
#define STATUS_PRIVILEGED_INSTRUCTION ((DWORD)0xC0000096L)
#define STATUS_STACK_OVERFLOW ((DWORD)0xC00000FDL)
#define STATUS_CONTROL_C_EXIT ((DWORD)0xC000013AL)
#define EXCEPTION_ACCESS_VIOLATION STATUS_ACCESS_VIOLATION
#define EXCEPTION_DATATYPE_MISALIGNMENT STATUS_DATATYPE_MISALIGNMENT
#define EXCEPTION_BREAKPOINT STATUS_BREAKPOINT
#define EXCEPTION_SINGLE_STEP STATUS_SINGLE_STEP
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED STATUS_ARRAY_BOUNDS_EXCEEDED
#define EXCEPTION_FLT_DENORMAL_OPERAND STATUS_FLOAT_DENORMAL_OPERAND
#define EXCEPTION_FLT_DIVIDE_BY_ZERO STATUS_FLOAT_DIVIDE_BY_ZERO
#define EXCEPTION_FLT_INEXACT_RESULT STATUS_FLOAT_INEXACT_RESULT
#define EXCEPTION_FLT_INVALID_OPERATION STATUS_FLOAT_INVALID_OPERATION
#define EXCEPTION_FLT_OVERFLOW STATUS_FLOAT_OVERFLOW
#define EXCEPTION_FLT_STACK_CHECK STATUS_FLOAT_STACK_CHECK
#define EXCEPTION_FLT_UNDERFLOW STATUS_FLOAT_UNDERFLOW
#define EXCEPTION_INT_DIVIDE_BY_ZERO STATUS_INTEGER_DIVIDE_BY_ZERO
#define EXCEPTION_INT_OVERFLOW STATUS_INTEGER_OVERFLOW
#define EXCEPTION_PRIV_INSTRUCTION STATUS_PRIVILEGED_INSTRUCTION
#define EXCEPTION_IN_PAGE_ERROR STATUS_IN_PAGE_ERROR
#define EXCEPTION_ILLEGAL_INSTRUCTION STATUS_ILLEGAL_INSTRUCTION
#define EXCEPTION_NONCONTINUABLE_EXCEPTION STATUS_NONCONTINUABLE_EXCEPTION
#define EXCEPTION_STACK_OVERFLOW STATUS_STACK_OVERFLOW
#define EXCEPTION_INVALID_DISPOSITION STATUS_INVALID_DISPOSITION
#define EXCEPTION_GUARD_PAGE STATUS_GUARD_PAGE_VIOLATION
#define EXCEPTION_INVALID_HANDLE STATUS_INVALID_HANDLE
#define EXCEPTION_NONCONTINUABLE 0x1
#define EXCEPTION_UNWINDING 0x2
#define EXCEPTION_EXIT_UNWIND 0x4
#define EXCEPTION_STACK_INVALID 0x8
#define EXCEPTION_NESTED_CALL 0x10
#define EXCEPTION_TARGET_UNWIND 0x20
#define EXCEPTION_COLLIDED_UNWIND 0x40
#define EXCEPTION_UNWIND (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | EXCEPTION_TARGET_UNWIND | EXCEPTION_COLLIDED_UNWIND)
#define EXCEPTION_MAXIMUM_PARAMETERS 15
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define abnormal_termination() 0
typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution,
    ExceptionContinueSearch,
    ExceptionNestedException,
    ExceptionCollidedUnwind
} EXCEPTION_DISPOSITION;

#define SIZE_OF_80387_REGISTERS 80
#define MAXIMUM_SUPPORTED_EXTENSION 512
#define CONTEXT_i386 0x00010000
#define CONTEXT_CONTROL (CONTEXT_i386 | 0x00000001L)
#define CONTEXT_INTEGER (CONTEXT_i386 | 0x00000002L)
#define CONTEXT_SEGMENTS (CONTEXT_i386 | 0x00000004L)
#define CONTEXT_FLOATING_POINT (CONTEXT_i386 | 0x00000008L)
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_i386 | 0x00000010L)
#define CONTEXT_EXTENDED_REGISTERS (CONTEXT_i386 | 0x00000020L)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)
#define CONTEXT_ALL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT | \
                     CONTEXT_DEBUG_REGISTERS | CONTEXT_EXTENDED_REGISTERS)
typedef struct _FLOATING_SAVE_AREA {
    DWORD ControlWord;
    DWORD StatusWord;
    DWORD TagWord;
    DWORD ErrorOffset;
    DWORD ErrorSelector;
    DWORD DataOffset;
    DWORD DataSelector;
    BYTE RegisterArea[SIZE_OF_80387_REGISTERS];
    DWORD Cr0NpxState;
} FLOATING_SAVE_AREA, *PFLOATING_SAVE_AREA;
typedef struct _CONTEXT {
    DWORD ContextFlags;
    DWORD Dr0;
    DWORD Dr1;
    DWORD Dr2;
    DWORD Dr3;
    DWORD Dr6;
    DWORD Dr7;
    FLOATING_SAVE_AREA FloatSave;
    DWORD SegGs;
    DWORD SegFs;
    DWORD SegEs;
    DWORD SegDs;
    DWORD Edi;
    DWORD Esi;
    DWORD Ebx;
    DWORD Edx;
    DWORD Ecx;
    DWORD Eax;
    DWORD Ebp;
    DWORD Eip;
    DWORD SegCs;
    DWORD EFlags;
    DWORD Esp;
    DWORD SegSs;
    BYTE ExtendedRegisters[MAXIMUM_SUPPORTED_EXTENSION];
} CONTEXT, *PCONTEXT, *LPCONTEXT;
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode;
    DWORD ExceptionFlags;
    struct _EXCEPTION_RECORD* ExceptionRecord;
    PVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD, *PEXCEPTION_RECORD, *LPEXCEPTION_RECORD;
typedef struct _EXCEPTION_POINTERS {
    PEXCEPTION_RECORD ExceptionRecord;
    PCONTEXT ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS, *LPEXCEPTION_POINTERS;
typedef EXCEPTION_DISPOSITION __cdecl EXCEPTION_ROUTINE(struct _EXCEPTION_RECORD* ExceptionRecord, PVOID EstablisherFrame,
                                                         struct _CONTEXT* ContextRecord, PVOID DispatcherContext);
typedef EXCEPTION_ROUTINE* PEXCEPTION_ROUTINE;
typedef struct _EXCEPTION_REGISTRATION_RECORD {
    struct _EXCEPTION_REGISTRATION_RECORD* Next;
    PEXCEPTION_ROUTINE Handler;
} EXCEPTION_REGISTRATION_RECORD, *PEXCEPTION_REGISTRATION_RECORD;
typedef LONG(__stdcall* PVECTORED_EXCEPTION_HANDLER)(struct _EXCEPTION_POINTERS* ExceptionInfo);
typedef LONG(__stdcall* PTOP_LEVEL_EXCEPTION_FILTER)(struct _EXCEPTION_POINTERS* ExceptionInfo);
typedef PTOP_LEVEL_EXCEPTION_FILTER LPTOP_LEVEL_EXCEPTION_FILTER;

// the thread information block at fs:[0] (agent C's w32_seh.cpp makes one per thread on Linux)
typedef struct _NT_TIB {
    struct _EXCEPTION_REGISTRATION_RECORD* ExceptionList;
    PVOID StackBase;
    PVOID StackLimit;
    PVOID SubSystemTib;
    union {
        PVOID FiberData;
        DWORD Version;
    };
    PVOID ArbitraryUserPointer;
    struct _NT_TIB* Self;
} NT_TIB, *PNT_TIB;
struct _TEB;
static inline struct _TEB* NtCurrentTeb(void) { return (struct _TEB*)__readfsdword(0x18); }

// ---- the PE image (winnt.h): the loader's and the import patchers' structures -------------------------------------------------------
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10b
#define IMAGE_FILE_MACHINE_I386 0x014c
#define IMAGE_FILE_RELOCS_STRIPPED 0x0001
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002
#define IMAGE_FILE_DLL 0x2000
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_SIZEOF_SHORT_NAME 8
#define IMAGE_DIRECTORY_ENTRY_EXPORT 0
#define IMAGE_DIRECTORY_ENTRY_IMPORT 1
#define IMAGE_DIRECTORY_ENTRY_RESOURCE 2
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION 3
#define IMAGE_DIRECTORY_ENTRY_SECURITY 4
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5
#define IMAGE_DIRECTORY_ENTRY_DEBUG 6
#define IMAGE_DIRECTORY_ENTRY_TLS 9
#define IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG 10
#define IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT 11
#define IMAGE_DIRECTORY_ENTRY_IAT 12
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#define IMAGE_SCN_CNT_CODE 0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_DISCARDABLE 0x02000000
#define IMAGE_SCN_MEM_SHARED 0x10000000
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000
#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGHLOW 3
#define IMAGE_ORDINAL_FLAG32 0x80000000
#define IMAGE_ORDINAL_FLAG IMAGE_ORDINAL_FLAG32
#define IMAGE_ORDINAL32(o) ((o) & 0xffff)
#define IMAGE_ORDINAL(o) IMAGE_ORDINAL32(o)
#define IMAGE_SNAP_BY_ORDINAL32(o) (((o) & IMAGE_ORDINAL_FLAG32) != 0)
#define IMAGE_SNAP_BY_ORDINAL(o) IMAGE_SNAP_BY_ORDINAL32(o)
#define IMAGE_FIRST_SECTION(nt) ((PIMAGE_SECTION_HEADER)((ULONG_PTR)(nt) + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + \
                                                         ((PIMAGE_NT_HEADERS)(nt))->FileHeader.SizeOfOptionalHeader))
#define RT_ICON MAKEINTRESOURCEA(3)
#define RT_BITMAP MAKEINTRESOURCEA(2)
#define RT_GROUP_ICON MAKEINTRESOURCEA(14)
#define RT_STRING MAKEINTRESOURCEA(6)
#define RT_RCDATA MAKEINTRESOURCEA(10)

#pragma pack(push, 2)                               // (winnt.h packs the DOS header to 2, as Windows)
typedef struct _IMAGE_DOS_HEADER {
    WORD e_magic;
    WORD e_cblp;
    WORD e_cp;
    WORD e_crlc;
    WORD e_cparhdr;
    WORD e_minalloc;
    WORD e_maxalloc;
    WORD e_ss;
    WORD e_sp;
    WORD e_csum;
    WORD e_ip;
    WORD e_cs;
    WORD e_lfarlc;
    WORD e_ovno;
    WORD e_res[4];
    WORD e_oemid;
    WORD e_oeminfo;
    WORD e_res2[10];
    LONG e_lfanew;
} IMAGE_DOS_HEADER, *PIMAGE_DOS_HEADER;
#pragma pack(pop)
typedef struct _IMAGE_FILE_HEADER {
    WORD Machine;
    WORD NumberOfSections;
    DWORD TimeDateStamp;
    DWORD PointerToSymbolTable;
    DWORD NumberOfSymbols;
    WORD SizeOfOptionalHeader;
    WORD Characteristics;
} IMAGE_FILE_HEADER, *PIMAGE_FILE_HEADER;
typedef struct _IMAGE_DATA_DIRECTORY {
    DWORD VirtualAddress;
    DWORD Size;
} IMAGE_DATA_DIRECTORY, *PIMAGE_DATA_DIRECTORY;
typedef struct _IMAGE_OPTIONAL_HEADER {
    WORD Magic;
    BYTE MajorLinkerVersion;
    BYTE MinorLinkerVersion;
    DWORD SizeOfCode;
    DWORD SizeOfInitializedData;
    DWORD SizeOfUninitializedData;
    DWORD AddressOfEntryPoint;
    DWORD BaseOfCode;
    DWORD BaseOfData;
    DWORD ImageBase;
    DWORD SectionAlignment;
    DWORD FileAlignment;
    WORD MajorOperatingSystemVersion;
    WORD MinorOperatingSystemVersion;
    WORD MajorImageVersion;
    WORD MinorImageVersion;
    WORD MajorSubsystemVersion;
    WORD MinorSubsystemVersion;
    DWORD Win32VersionValue;
    DWORD SizeOfImage;
    DWORD SizeOfHeaders;
    DWORD CheckSum;
    WORD Subsystem;
    WORD DllCharacteristics;
    DWORD SizeOfStackReserve;
    DWORD SizeOfStackCommit;
    DWORD SizeOfHeapReserve;
    DWORD SizeOfHeapCommit;
    DWORD LoaderFlags;
    DWORD NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32, *PIMAGE_OPTIONAL_HEADER32;
typedef IMAGE_OPTIONAL_HEADER32 IMAGE_OPTIONAL_HEADER;
typedef PIMAGE_OPTIONAL_HEADER32 PIMAGE_OPTIONAL_HEADER;
typedef struct _IMAGE_NT_HEADERS {
    DWORD Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32, *PIMAGE_NT_HEADERS32;
typedef IMAGE_NT_HEADERS32 IMAGE_NT_HEADERS;
typedef PIMAGE_NT_HEADERS32 PIMAGE_NT_HEADERS;
typedef struct _IMAGE_SECTION_HEADER {
    BYTE Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        DWORD PhysicalAddress;
        DWORD VirtualSize;
    } Misc;
    DWORD VirtualAddress;
    DWORD SizeOfRawData;
    DWORD PointerToRawData;
    DWORD PointerToRelocations;
    DWORD PointerToLinenumbers;
    WORD NumberOfRelocations;
    WORD NumberOfLinenumbers;
    DWORD Characteristics;
} IMAGE_SECTION_HEADER, *PIMAGE_SECTION_HEADER;
typedef struct _IMAGE_IMPORT_DESCRIPTOR {
    union {
        DWORD Characteristics;
        DWORD OriginalFirstThunk;
    };
    DWORD TimeDateStamp;
    DWORD ForwarderChain;
    DWORD Name;
    DWORD FirstThunk;
} IMAGE_IMPORT_DESCRIPTOR, *PIMAGE_IMPORT_DESCRIPTOR;
typedef struct _IMAGE_IMPORT_BY_NAME {
    WORD Hint;
    CHAR Name[1];
} IMAGE_IMPORT_BY_NAME, *PIMAGE_IMPORT_BY_NAME;
typedef struct _IMAGE_THUNK_DATA32 {
    union {
        DWORD ForwarderString;
        DWORD Function;
        DWORD Ordinal;
        DWORD AddressOfData;
    } u1;
} IMAGE_THUNK_DATA32, *PIMAGE_THUNK_DATA32;
typedef IMAGE_THUNK_DATA32 IMAGE_THUNK_DATA;
typedef PIMAGE_THUNK_DATA32 PIMAGE_THUNK_DATA;
typedef struct _IMAGE_EXPORT_DIRECTORY {
    DWORD Characteristics;
    DWORD TimeDateStamp;
    WORD MajorVersion;
    WORD MinorVersion;
    DWORD Name;
    DWORD Base;
    DWORD NumberOfFunctions;
    DWORD NumberOfNames;
    DWORD AddressOfFunctions;
    DWORD AddressOfNames;
    DWORD AddressOfNameOrdinals;
} IMAGE_EXPORT_DIRECTORY, *PIMAGE_EXPORT_DIRECTORY;
typedef struct _IMAGE_BASE_RELOCATION {
    DWORD VirtualAddress;
    DWORD SizeOfBlock;
} IMAGE_BASE_RELOCATION, *PIMAGE_BASE_RELOCATION;
typedef struct _IMAGE_RESOURCE_DIRECTORY {
    DWORD Characteristics;
    DWORD TimeDateStamp;
    WORD MajorVersion;
    WORD MinorVersion;
    WORD NumberOfNamedEntries;
    WORD NumberOfIdEntries;
} IMAGE_RESOURCE_DIRECTORY, *PIMAGE_RESOURCE_DIRECTORY;
typedef struct _IMAGE_RESOURCE_DIRECTORY_ENTRY {
    DWORD Name;                                   // (a name's offset | 0x80000000, or an id)
    DWORD OffsetToData;                           // (a subdirectory's offset | 0x80000000, or a data entry's)
} IMAGE_RESOURCE_DIRECTORY_ENTRY, *PIMAGE_RESOURCE_DIRECTORY_ENTRY;
typedef struct _IMAGE_RESOURCE_DATA_ENTRY {
    DWORD OffsetToData;
    DWORD Size;
    DWORD CodePage;
    DWORD Reserved;
} IMAGE_RESOURCE_DATA_ENTRY, *PIMAGE_RESOURCE_DATA_ENTRY;
#define IMAGE_RESOURCE_NAME_IS_STRING 0x80000000
#define IMAGE_RESOURCE_DATA_IS_DIRECTORY 0x80000000

// ---- windows, messages, GDI (windef.h, winuser.h, wingdi.h) ----------------------------------------------------------------------
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT, *LPRECT;
typedef const RECT* LPCRECT;
typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT, *PPOINT, *LPPOINT;
typedef struct tagSIZE {
    LONG cx;
    LONG cy;
} SIZE, *PSIZE, *LPSIZE;
typedef struct tagPALETTEENTRY {
    BYTE peRed;
    BYTE peGreen;
    BYTE peBlue;
    BYTE peFlags;
} PALETTEENTRY, *PPALETTEENTRY, *LPPALETTEENTRY;
typedef struct tagRGBQUAD {
    BYTE rgbBlue;
    BYTE rgbGreen;
    BYTE rgbRed;
    BYTE rgbReserved;
} RGBQUAD;
#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1;
    WORD bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER, *PBITMAPFILEHEADER;
#pragma pack(pop)
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;
typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;
#define BI_RGB 0L
#define DIB_RGB_COLORS 0
#define SRCCOPY (DWORD)0x00CC0020

typedef LRESULT(__stdcall* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LRESULT(__stdcall* HOOKPROC)(int code, WPARAM wParam, LPARAM lParam);
typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *PMSG, *LPMSG;
typedef struct tagWNDCLASSA {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASSA, *PWNDCLASSA, *LPWNDCLASSA;
typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *PPAINTSTRUCT, *LPPAINTSTRUCT;

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_SETICON 0x0080
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_USER 0x0400
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define SC_KEYMENU 0xF100
#define SC_TASKLIST 0xF130
#define SC_SCREENSAVE 0xF140
#define SC_MONITORPOWER 0xF170
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOW 5
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_OWNDC 0x0020
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_VISIBLE 0x10000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_TOOLWINDOW 0x00000080L
#define GWL_WNDPROC (-4)
#define GWLP_WNDPROC (-4)
#define GWL_STYLE (-16)
#define ICON_SMALL 0
#define ICON_BIG 1
#define IDC_ARROW MAKEINTRESOURCEA(32512)
#define IDI_APPLICATION MAKEINTRESOURCEA(32512)
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_YESNO 0x00000004L
#define MB_ICONHAND 0x00000010L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONINFORMATION 0x00000040L
#define MB_TOPMOST 0x00040000L
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define IDNO 7
#define MOUSEEVENTF_MOVE 0x0001
#define MOUSEEVENTF_ABSOLUTE 0x8000
#define CF_TEXT 1
#define WH_KEYBOARD 2
#define WH_GETMESSAGE 3

// ---- multimedia (mmsystem.h) ------------------------------------------------------------------------------------------------
typedef UINT MMRESULT;
#define TIMERR_NOERROR 0
#define WAVE_FORMAT_PCM 1
#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
#pragma pack(pop)
typedef const WAVEFORMATEX* LPCWAVEFORMATEX;

// ---- COM's basics (guiddef.h, unknwn.h) ---------------------------------------------------------------------------------------
typedef struct _GUID {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
} GUID;
typedef GUID IID;
typedef GUID CLSID;
typedef GUID* LPGUID;
typedef const GUID* LPCGUID;
typedef IID* LPIID;
typedef CLSID* LPCLSID;
#define REFGUID const GUID&
#define REFIID const IID&
#define REFCLSID const IID&
inline bool IsEqualGUID(REFGUID a, REFGUID b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }
#define IsEqualIID(a, b) IsEqualGUID(a, b)
inline bool operator==(REFGUID a, REFGUID b) { return IsEqualGUID(a, b); }
inline bool operator!=(REFGUID a, REFGUID b) { return !IsEqualGUID(a, b); }
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    extern "C" const GUID name __attribute__((weak)) = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}
// IUnknown: the vtable's first three slots, __stdcall (this on the stack), as every COM interface the game sees
struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
typedef IUnknown* LPUNKNOWN;

// ---- Interlocked (winbase.h): full barriers, on LONG ---------------------------------------------------------------------------
static inline LONG InterlockedIncrement(LONG volatile* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(LONG volatile* p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(LONG volatile* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(LONG volatile* p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(LONG volatile* p, LONG v, LONG cmp) {
    __atomic_compare_exchange_n(p, &cmp, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

// ---- the Windows functions the port names ------------------------------------------------------------------------------------------
// Windows' prototypes (__stdcall, C names). Most are named only for their type -- decltype(&HeapAlloc) is how the
// port calls race.exe's import slot, which the stand-ins fill on Linux -- and need no definition. One the Linux build
// really calls is defined by the port (the stand-ins, vp_os.cpp) or, until agents B/C/D replace those calls, stubbed
// in hook/linux_todo.cpp (vp_r2b_todo).
typedef CHAR* LPCH;
typedef const CHAR* LPCCH;
typedef WCHAR* LPWCH;
typedef const WCHAR* LPCWCH;
typedef struct tagLOGPALETTE LOGPALETTE;
typedef struct _currencyfmtA CURRENCYFMTA;
typedef struct _OSVERSIONINFOA* LPOSVERSIONINFOA;
typedef struct _CONSOLE_READCONSOLE_CONTROL* PCONSOLE_READCONSOLE_CONTROL;
extern "C" {
PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
BOOL WINAPI AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle);
BOOL WINAPI AllocConsole(void);
HDC WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);
BOOL WINAPI BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);
LRESULT WINAPI CallNextHookEx(HHOOK hhk, int nCode, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
BOOL WINAPI ClipCursor(const RECT *lpRect);
BOOL WINAPI CloseClipboard(void);
BOOL WINAPI CloseHandle(HANDLE hObject);
BOOL WINAPI CopyFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists);
HDC WINAPI CreateCompatibleDC(HDC hdc);
BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
    LPCSTR lpName);
HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile);
HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
    DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
HANDLE WINAPI CreateMailslotA(LPCSTR lpName, DWORD nMaxMessageSize, DWORD lReadTimeout,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes);
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HPALETTE WINAPI CreatePalette(const LOGPALETTE *plpal);
BOOL WINAPI CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
    LPCSTR lpName);
HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
HWND WINAPI CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y,
    int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
LRESULT WINAPI DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
VOID WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI DeleteDC(HDC hdc);
BOOL WINAPI DeleteFileA(LPCSTR lpFileName);
BOOL WINAPI DeleteObject(HGDIOBJ ho);
BOOL WINAPI DestroyWindow(HWND hWnd);
BOOL WINAPI DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode, LPVOID lpInBuffer, DWORD nInBufferSize,
    LPVOID lpOutBuffer, DWORD nOutBufferSize, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped);
BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule);
LRESULT WINAPI DispatchMessageA(const MSG *lpMsg);
BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
    LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);
BOOL WINAPI EmptyClipboard(void);
BOOL WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint);
VOID WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
__attribute__((noreturn)) VOID WINAPI ExitProcess(UINT uExitCode);
__attribute__((noreturn)) VOID WINAPI ExitThread(DWORD dwExitCode);
BOOL WINAPI FindClose(HANDLE hFindFile);
HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
HWND WINAPI FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName);
BOOL WINAPI FlushFileBuffers(HANDLE hFile);
BOOL WINAPI FlushInstructionCache(HANDLE hProcess, LPCVOID lpBaseAddress, SIZE_T dwSize);
BOOL WINAPI FreeConsole(void);
BOOL WINAPI FreeEnvironmentStringsA(LPCH penv);
BOOL WINAPI FreeEnvironmentStringsW(LPWCH penv);
UINT WINAPI GetACP(void);
HWND WINAPI GetActiveWindow(void);
BOOL WINAPI GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo);
HANDLE WINAPI GetClipboardData(UINT uFormat);
LPSTR WINAPI GetCommandLineA(void);
int WINAPI GetCurrencyFormatA(LCID Locale, DWORD dwFlags, LPCSTR lpValue, const CURRENCYFMTA *lpFormat,
    LPSTR lpCurrencyStr, int cchCurrency);
DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer);
HANDLE WINAPI GetCurrentProcess(void);
HANDLE WINAPI GetCurrentThread(void);
DWORD WINAPI GetCurrentThreadId(void);
HDC WINAPI GetDC(HWND hWnd);
UINT WINAPI GetDriveTypeA(LPCSTR lpRootPathName);
LPWCH WINAPI GetEnvironmentStringsW(void);
DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName);
DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
DWORD WINAPI GetFileType(HANDLE hFile);
HWND WINAPI GetForegroundWindow(void);
DWORD WINAPI GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR *lpFilePart);
HWND WINAPI GetLastActivePopup(HWND hWnd);
DWORD WINAPI GetLastError(void);
VOID WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
int WINAPI GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData);
BOOL WINAPI GetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
BOOL WINAPI GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule);
UINT WINAPI GetOEMCP(void);
HWND WINAPI GetParent(HWND hWnd);
int WINAPI GetPixelFormat(HDC hdc);
UINT WINAPI GetPrivateProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName, INT nDefault, LPCSTR lpFileName);
DWORD WINAPI GetPrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
    DWORD nSize, LPCSTR lpFileName);
FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
BOOL WINAPI GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask);
BOOL WINAPI GetProcessDEPPolicy(HANDLE hProcess, LPDWORD lpFlags, PBOOL lpPermanent);
VOID WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
BOOL WINAPI GetStringTypeA(LCID Locale, DWORD dwInfoType, LPCSTR lpSrcStr, int cchSrc, LPWORD lpCharType);
BOOL WINAPI GetStringTypeW(DWORD dwInfoType, LPCWCH lpSrcStr, int cchSrc, LPWORD lpCharType);
UINT WINAPI GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize);
VOID WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
int WINAPI GetSystemMetrics(int nIndex);
UINT WINAPI GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName);
DWORD WINAPI GetTickCount(void);
DWORD WINAPI GetVersion(void);
BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation);
HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes);
HGLOBAL WINAPI GlobalFree(HGLOBAL hMem);
LPVOID WINAPI GlobalLock(HGLOBAL hMem);
SIZE_T WINAPI GlobalSize(HGLOBAL hMem);
BOOL WINAPI GlobalUnlock(HGLOBAL hMem);
LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);
HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize);
BOOL WINAPI HeapDestroy(HANDLE hHeap);
BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
LPVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);
SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
BOOL WINAPI HeapValidate(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
VOID WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI IsBadReadPtr(const VOID *lp, UINT_PTR ucb);
BOOL WINAPI IsWindow(HWND hWnd);
int WINAPI LCMapStringA(LCID Locale, DWORD dwMapFlags, LPCSTR lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest);
int WINAPI LCMapStringW(LCID Locale, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest);
VOID WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
HBITMAP WINAPI LoadBitmapA(HINSTANCE hInstance, LPCSTR lpBitmapName);
HCURSOR WINAPI LoadCursorA(HINSTANCE hInstance, LPCSTR lpCursorName);
HICON WINAPI LoadIconA(HINSTANCE hInstance, LPCSTR lpIconName);
HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
LPVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
    DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
UINT WINAPI MapVirtualKeyA(UINT uCode, UINT uMapType);
int WINAPI MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
    LPWSTR lpWideCharStr, int cchWideChar);
BOOL WINAPI OpenClipboard(HWND hWndNewOwner);
BOOL WINAPI PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);
VOID WINAPI PostQuitMessage(int nExitCode);
BOOL WINAPI PulseEvent(HANDLE hEvent);
BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount);
BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency);
VOID WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
    const ULONG_PTR *lpArguments);
BOOL WINAPI ReadConsoleA(HANDLE console_input, LPVOID buffer, DWORD number_of_chars_to_read,
    LPDWORD number_of_chars_read, PCONSOLE_READCONSOLE_CONTROL input_control);
BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped);
LONG WINAPI RegFlushKey(HKEY hKey);
ATOM WINAPI RegisterClassA(const WNDCLASSA *lpWndClass);
int WINAPI ReleaseDC(HWND hWnd, HDC hDC);
BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, LPLONG lpPreviousCount);
BOOL WINAPI ResetEvent(HANDLE hEvent);
DWORD WINAPI ResumeThread(HANDLE hThread);
HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ h);
LRESULT WINAPI SendMessageA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
HANDLE WINAPI SetClipboardData(UINT uFormat, HANDLE hMem);
BOOL WINAPI SetConsoleMode(HANDLE console_handle, DWORD mode);
BOOL WINAPI SetCurrentDirectoryA(LPCSTR lpPathName);
HCURSOR WINAPI SetCursor(HCURSOR hCursor);
BOOL WINAPI SetCursorPos(int X, int Y);
BOOL WINAPI SetEndOfFile(HANDLE hFile);
BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue);
BOOL WINAPI SetEvent(HANDLE hEvent);
BOOL WINAPI SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes);
DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
HWND WINAPI SetFocus(HWND hWnd);
BOOL WINAPI SetForegroundWindow(HWND hWnd);
UINT WINAPI SetHandleCount(UINT uNumber);
VOID WINAPI SetLastError(DWORD dwErrCode);
BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle);
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask);
BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
HHOOK WINAPI SetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hmod, DWORD dwThreadId);
int WINAPI ShowCursor(BOOL bShow);
BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
VOID WINAPI Sleep(DWORD dwMilliseconds);
DWORD WINAPI SuspendThread(HANDLE hThread);
BOOL WINAPI TranslateMessage(const MSG *lpMsg);
LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo);
BOOL WINAPI UnhookWindowsHookEx(HHOOK hhk);
BOOL WINAPI UnmapViewOfFile(LPCVOID lpBaseAddress);
BOOL WINAPI UpdateWindow(HWND hWnd);
LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength);
DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
    LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar);
BOOL WINAPI WriteConsoleA(HANDLE console_output, const void *buffer, DWORD number_of_chars_to_write,
    LPDWORD number_of_chars_written, LPVOID reserved);
BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped);
LPSTR WINAPI lstrcatA(LPSTR lpString1, LPCSTR lpString2);
LPSTR WINAPI lstrcpyA(LPSTR lpString1, LPCSTR lpString2);
LPSTR WINAPI lstrcpynA(LPSTR lpString1, LPCSTR lpString2, int iMaxLength);
VOID WINAPI mouse_event(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData, ULONG_PTR dwExtraInfo);
MMRESULT WINAPI timeBeginPeriod(UINT uPeriod);
MMRESULT WINAPI timeEndPeriod(UINT uPeriod);
DWORD WINAPI timeGetTime(void);
MMRESULT WINAPI timeKillEvent(UINT uTimerID);
int WINAPIV wsprintfA(LPSTR, LPCSTR, ...);
}

// ---- layouts (Windows' 32-bit sizes and offsets) ----------------------------------------------------------------------------------
static_assert(sizeof(DWORD) == 4 && sizeof(LONG) == 4 && sizeof(WCHAR) == 2 && sizeof(HANDLE) == 4 && sizeof(WPARAM) == 4,
              "Windows' base types");
static_assert(sizeof(LARGE_INTEGER) == 8 && alignof(LARGE_INTEGER) == 8, "LARGE_INTEGER");
static_assert(sizeof(SYSTEMTIME) == 16 && sizeof(FILETIME) == 8, "SYSTEMTIME / FILETIME");
static_assert(sizeof(CRITICAL_SECTION) == 0x18 && offsetof(CRITICAL_SECTION, OwningThread) == 0x0c, "CRITICAL_SECTION");
static_assert(sizeof(SECURITY_ATTRIBUTES) == 12, "SECURITY_ATTRIBUTES");
static_assert(sizeof(STARTUPINFOA) == 0x44 && offsetof(STARTUPINFOA, wShowWindow) == 0x30 &&
                  offsetof(STARTUPINFOA, hStdInput) == 0x38, "STARTUPINFOA");
static_assert(sizeof(PROCESS_INFORMATION) == 16, "PROCESS_INFORMATION");
static_assert(sizeof(SYSTEM_INFO) == 0x24 && offsetof(SYSTEM_INFO, dwPageSize) == 4 &&
                  offsetof(SYSTEM_INFO, wProcessorLevel) == 0x20, "SYSTEM_INFO");
static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 28 && offsetof(MEMORY_BASIC_INFORMATION, State) == 0x10, "MBI");
static_assert(sizeof(WIN32_FIND_DATAA) == 0x140 && offsetof(WIN32_FIND_DATAA, nFileSizeHigh) == 0x1c &&
                  offsetof(WIN32_FIND_DATAA, cFileName) == 0x2c && offsetof(WIN32_FIND_DATAA, cAlternateFileName) == 0x130,
              "WIN32_FIND_DATAA");
static_assert(sizeof(OVERLAPPED) == 20, "OVERLAPPED");
static_assert(sizeof(CPINFO) == 20, "CPINFO");
static_assert(sizeof(FLOATING_SAVE_AREA) == 112 && offsetof(FLOATING_SAVE_AREA, RegisterArea) == 0x1c, "FLOATING_SAVE_AREA");
static_assert(sizeof(CONTEXT) == 0x2cc && offsetof(CONTEXT, FloatSave) == 0x1c && offsetof(CONTEXT, SegGs) == 0x8c &&
                  offsetof(CONTEXT, Edi) == 0x9c && offsetof(CONTEXT, Eax) == 0xb0 && offsetof(CONTEXT, Ebp) == 0xb4 &&
                  offsetof(CONTEXT, Eip) == 0xb8 && offsetof(CONTEXT, EFlags) == 0xc0 && offsetof(CONTEXT, Esp) == 0xc4 &&
                  offsetof(CONTEXT, ExtendedRegisters) == 0xcc, "CONTEXT (i386)");
static_assert(sizeof(EXCEPTION_RECORD) == 0x50 && offsetof(EXCEPTION_RECORD, ExceptionAddress) == 0x0c &&
                  offsetof(EXCEPTION_RECORD, ExceptionInformation) == 0x14, "EXCEPTION_RECORD");
static_assert(sizeof(EXCEPTION_POINTERS) == 8 && sizeof(EXCEPTION_REGISTRATION_RECORD) == 8, "EXCEPTION_POINTERS");
static_assert(sizeof(NT_TIB) == 0x1c && offsetof(NT_TIB, StackBase) == 4 && offsetof(NT_TIB, StackLimit) == 8 &&
                  offsetof(NT_TIB, Self) == 0x18, "NT_TIB");
static_assert(sizeof(IMAGE_DOS_HEADER) == 64 && alignof(IMAGE_DOS_HEADER) == 2 && offsetof(IMAGE_DOS_HEADER, e_lfanew) == 0x3c,
              "IMAGE_DOS_HEADER");
static_assert(sizeof(IMAGE_FILE_HEADER) == 20, "IMAGE_FILE_HEADER");
static_assert(sizeof(IMAGE_OPTIONAL_HEADER32) == 224 && offsetof(IMAGE_OPTIONAL_HEADER32, ImageBase) == 28 &&
                  offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) == 96, "IMAGE_OPTIONAL_HEADER32");
static_assert(sizeof(IMAGE_NT_HEADERS32) == 248 && offsetof(IMAGE_NT_HEADERS32, OptionalHeader) == 24, "IMAGE_NT_HEADERS");
static_assert(sizeof(IMAGE_SECTION_HEADER) == 40, "IMAGE_SECTION_HEADER");
static_assert(sizeof(IMAGE_IMPORT_DESCRIPTOR) == 20 && offsetof(IMAGE_IMPORT_DESCRIPTOR, FirstThunk) == 16, "IMPORT_DESCRIPTOR");
static_assert(sizeof(IMAGE_EXPORT_DIRECTORY) == 40 && sizeof(IMAGE_BASE_RELOCATION) == 8, "IMAGE_EXPORT_DIRECTORY");
static_assert(sizeof(IMAGE_RESOURCE_DIRECTORY) == 16 && sizeof(IMAGE_RESOURCE_DATA_ENTRY) == 16, "IMAGE_RESOURCE_*");
static_assert(sizeof(RECT) == 16 && sizeof(POINT) == 8 && sizeof(PALETTEENTRY) == 4, "RECT / POINT");
static_assert(sizeof(BITMAPFILEHEADER) == 14 && sizeof(BITMAPINFOHEADER) == 40, "BITMAP*HEADER");
static_assert(sizeof(MSG) == 28 && offsetof(MSG, pt) == 20, "MSG");
static_assert(sizeof(WNDCLASSA) == 40 && sizeof(PAINTSTRUCT) == 64, "WNDCLASSA / PAINTSTRUCT");
static_assert(sizeof(WAVEFORMATEX) == 18, "WAVEFORMATEX");
static_assert(sizeof(GUID) == 16, "GUID");
