// krn_file.cpp -- M3 stage "kernel and utilities", group K2: the game's files, log, window shell, messages and
// shared memory, rewritten. kernel:file.obj (file I/O through a 32-slot table, directory search and creation,
// real and fake memory maps), kernel:log.obj (the log: its sinks, LogReport / LogError / LogPanic / LogHang /
// the asserts, and the monochrome debug monitor), kernel:win32.obj (the single-instance check, the user
// directory, the splash window, the old window procedure, the cursor clip, the registry flush, message hooks,
// Win32System ...), kernel:winerror.obj (Win32GetErrorString: a table of 565 error names), kernel:msg.obj (a
// message client / server over mailslots) and kernel:shmem.obj (triple-buffered blocks shared between tasks).
//
// Written from the v1.0 disassembly, faithful: every call in the original's order; then three fixes, each marked
// `// FIX:` (docs/FIXES.md): the user directory and the log moved next to race.exe, and FileReadLine's bare-LF
// lines. A Windows API the original calls through its import table (`call dword ptr [0x5d7xxx]`) is called through
// THAT game IAT slot (IAT() below), never through this DLL's own imports -- the DLL patches some game imports, Wine
// may too, and a harness can stub a slot. The C runtime functions it calls (vsprintf, strncpy, strchr, stricmp,
// memmove, isspace, chdir: the game's statically linked LIBC, with its own locale state) are called at their game
// addresses; the ones the 1998 compiler inlined (strcpy, strcat, strlen, memset: rep movs / repne scas / rep
// stos) are done here. Game functions -- this group's own too -- are called by their v1.0 address, so a hooked
// rewrite or the original is what runs. Callbacks and window procedures are passed as the originals' addresses
// (splash_proc 0x412570, the log sinks 0x410f10 / 0x410ff0 ...), as the original does.
//
// The statics (v1.0 addresses):
//   log.obj   0x4e5a1c mono_off (GetVersion says NT: the mono monitor is never written), 0x4e5a20 / 0x4e5a24 the
//             log and end sinks (noop_log / file_log / msg_log and noop_end / file_end / msg_end), 0x4e5a28 bytes
//             in the log file (it stops at 1,000,000), 0x4e5a2c "maximum size" written, 0x4e5a30 / 0x4e5a34 the
//             mono cursor (only mono_clear touches them), 0x505e58 the "Log" and 0x505e98 the "Mono" Multi
//             locks, 0x505e5c the mono line log() prints on (0..24), 0x505e60 the log file (a File number),
//             0x505e68 the hook count and 0x505e6c the 10 hooks.
//   file.obj  0x4e5c54 the "File" Multi lock, 0x4e5c58 FileCreateTemp's counter, 0x505ea0 how many of the 32
//             slots have ever been used, 0x505ea8 the 32 slots (M1: 256 in the DLL) {char name[0x100]; HANDLE handle} (-1 free). A
//             File is a slot's index + 1 (0: failed).
//   win32.obj 0x4e5eac the game window, 0x4e5eb0 the window in front before it, 0x4e5eb4 the instance, 0x4e5eb8
//             the command line, 0x4e5ebc the keyboard hook, 0x4e5ec0 inactive (switched away), 0x4e5ec4 the
//             single-instance semaphore, 0x4e5ec8 console mode, 0x4e5ecc the loading window, 0x507f30 its bitmap,
//             0x507f48 the user directory (0x104), 0x508060 the two message hooks; 0x4db438 / 0x4db43c / 0x4db440
//             the semaphore name, window class and title (pointers M2 reads too).
//   msg.obj   0x508d38 the message {int seq; char text[0x100]} (0x104 bytes on the wire), 0x508e3c the mailslot
//             (client and server share it).
//   shmem.obj 0x503a74 the block count, 0x5d5768 the blocks (9 slots before shmem_mutex at 0x5d578c: no bound).
//
// Threads: the log runs on both threads (LogReport and friends take the "Log" Multi lock; the physics thread logs
// too), shmem is how the physics task hands its state packets to the main thread (PhysTaskBegin allocates,
// PhysicsStart gets), everything else runs on the main thread. Every footprint lists the statics its function
// writes; whatever does I/O, allocates, takes a lock, creates or talks to windows, or ends the game is replay_only.
//
// Who calls what, and what is dead in v1.0: nothing calls FileSeekAbsolute / FileSeekRelative (they only
// panic), FileCreateTemp, FileOpenWritable, LogHang, Win32IsConsoleMode, MsgServerBegin / Recv / End. win32_event
// and key_hook_proc are the original create_window's window procedure and hook_keys' keyboard hook: with M2's SDL
// window ([platform] sdl=1) neither is ever installed, so both are dead there (live with sdl=0).
// make_loading_window / splash_proc are WinMain's splash. Win32System is GhostCarViewer::set_driver_ghost's.
//
// Skipped: the $E initialisers ($E1/$E2 of each object jump to rcfunc_is_internal, a bare `ret`; win32.obj's
// $E5..$E50 store 16 colour constants; the CRT runs them before anything could be hooked), and the bare `ret`s
// noop_log (0x410cf0), noop_end (0x410d00), log_devices (0x412b20) and the three ASSERT_MSGs (file.obj 0x411ae0,
// msg.obj 0x415e70, shmem.obj 0x4d8650) -- still called, by address, where the originals call them.
//
// Variadic functions (LogReport, LogError, LogPanic, LogHang, LogAssert, LogAssertMsg, FilePrintf, Win32System)
// are written with a `Va` parameter in place of `...`: a window of 32 dwords over the caller's pushed arguments
// (cdecl: the caller pushes them and pops them), whose address is the va_list. The shadow wrapper forwards the
// same window to the original, so both see the caller's arguments.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "fix_paths.h"

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// ---- the game's import table -----------------------------------------------------------------------------------------
// a Windows API through the game's IAT slot, read at the call (the original's `call dword ptr [slot]`)
#define IAT(slot, fn) (*(decltype(&fn) volatile*)(uintptr_t)(slot))
typedef DWORD(WINAPI* GetVersion_t)(void);            // (GetVersion is declared deprecated)
#define kGetVersion          (*(GetVersion_t volatile*)(uintptr_t)0x005d7558)
#define kCreateFileA         IAT(0x005d7554, CreateFileA)
#define kGetTempFileNameA    IAT(0x005d7550, GetTempFileNameA)
#define kSetFilePointer      IAT(0x005d754c, SetFilePointer)
#define kCloseHandle         IAT(0x005d7548, CloseHandle)
#define kGetFileSize         IAT(0x005d7544, GetFileSize)
#define kGetLastError        IAT(0x005d7540, GetLastError)
#define kReadFile            IAT(0x005d753c, ReadFile)
#define kWriteFile           IAT(0x005d7538, WriteFile)
#define kDuplicateHandle     IAT(0x005d7534, DuplicateHandle)
#define kGetCurrentProcess   IAT(0x005d7530, GetCurrentProcess)
#define kSetFileAttributesA  IAT(0x005d74e0, SetFileAttributesA)
#define kDeleteFileA         IAT(0x005d74d8, DeleteFileA)
#define kFindFirstFileA      IAT(0x005d74c8, FindFirstFileA)
#define kFindNextFileA       IAT(0x005d74dc, FindNextFileA)
#define kFindClose           IAT(0x005d748c, FindClose)
#define kCreateDirectoryA    IAT(0x005d7490, CreateDirectoryA)
#define kCopyFileA           IAT(0x005d7460, CopyFileA)
#define kCreateFileMappingA  IAT(0x005d7468, CreateFileMappingA)
#define kMapViewOfFile       IAT(0x005d7464, MapViewOfFile)
#define kUnmapViewOfFile     IAT(0x005d746c, UnmapViewOfFile)
#define kCreateSemaphoreA    IAT(0x005d7470, CreateSemaphoreA)
#define kCreateProcessA      IAT(0x005d7484, CreateProcessA)
#define kWaitForSingleObject IAT(0x005d7480, WaitForSingleObject)
#define kGetExitCodeProcess  IAT(0x005d747c, GetExitCodeProcess)
#define kCreateMailslotA     IAT(0x005d74e8, CreateMailslotA)
#define kMessageBoxA         IAT(0x005d767c, MessageBoxA)
#define kFindWindowA         IAT(0x005d7664, FindWindowA)
#define kShowWindow          IAT(0x005d7668, ShowWindow)
#define kGetSystemMetrics    IAT(0x005d7644, GetSystemMetrics)
#define kLoadCursorA         IAT(0x005d765c, LoadCursorA)
#define kRegisterClassA      IAT(0x005d764c, RegisterClassA)
#define kCreateWindowExA     IAT(0x005d7650, CreateWindowExA)
#define kUpdateWindow        IAT(0x005d7660, UpdateWindow)
#define kLoadBitmapA         IAT(0x005d7690, LoadBitmapA)
#define kBeginPaint          IAT(0x005d76c8, BeginPaint)
#define kEndPaint            IAT(0x005d7648, EndPaint)
#define kDefWindowProcA      IAT(0x005d7694, DefWindowProcA)
#define kShowCursor          IAT(0x005d76b0, ShowCursor)
#define kPostQuitMessage     IAT(0x005d76c0, PostQuitMessage)
#define kSetCursor           IAT(0x005d76c4, SetCursor)
#define kClipCursor          IAT(0x005d76b4, ClipCursor)
#define kCallNextHookEx      IAT(0x005d7674, CallNextHookEx)
#define kDestroyWindow       IAT(0x005d7678, DestroyWindow)
#define kSetForegroundWindow IAT(0x005d76cc, SetForegroundWindow)
#define kUnhookWindowsHookEx IAT(0x005d76a4, UnhookWindowsHookEx)
#define kDeleteObject        IAT(0x005d7448, DeleteObject)
#define kCreateCompatibleDC  IAT(0x005d744c, CreateCompatibleDC)
#define kSelectObject        IAT(0x005d7454, SelectObject)
#define kBitBlt              IAT(0x005d7444, BitBlt)
#define kDeleteDC            IAT(0x005d7450, DeleteDC)
#define kRegFlushKey         IAT(0x005d7410, RegFlushKey)
#define kGetFullPathNameA    IAT(0x005d7594, GetFullPathNameA)   // (the user directory's FIX)
typedef DWORD(WINAPI* TimeGetTime_t)(void);           // (winmm, not included here)
#define kTimeGetTime         (*(TimeGetTime_t volatile*)(uintptr_t)0x005d76d8)

// ---- the variadic window (see the top) --------------------------------------------------------------------------------
namespace {
struct Va {
    uint32_t w[32];
    Va() = default;
    explicit Va(uint32_t) {}                        // (test/fuzz.h builds arguments with a cast)
};
}  // namespace
#define VA(v) ((va_list)(void*)&(v))

// ---- the game's C runtime ---------------------------------------------------------------------------------------------
typedef int(__cdecl* Vsprintf_t)(char*, const char*, va_list);
static const Vsprintf_t vsprintf_o = (Vsprintf_t)0x004cf7c0;
typedef char*(__cdecl* Strncpy_t)(char*, const char*, size_t);
static const Strncpy_t strncpy_o = (Strncpy_t)0x004cf3a0;
typedef char*(__cdecl* Strchr_t)(const char*, int);
static const Strchr_t strchr_o = (Strchr_t)0x004ce0c0;
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t stricmp_o = (Stricmp_t)0x004da350;
typedef void*(__cdecl* Memmove_t)(void*, const void*, size_t);
static const Memmove_t memmove_o = (Memmove_t)0x004cf400;
typedef int(__cdecl* Isspace_t)(int);
static const Isspace_t isspace_o = (Isspace_t)0x004cf790;
typedef int(__cdecl* Chdir_t)(const char*);
static const Chdir_t chdir_o = (Chdir_t)0x004cf830;

// the inlined string code: strlen (repne scasb), strcpy (rep movsd; rep movsb: strlen + 1 bytes, forward)
static __forceinline uint32_t str_len(const char* s) { return (uint32_t)strlen(s); }
static __forceinline void str_copy(char* dst, const char* src) {
    uint32_t n = str_len(src) + 1;
#ifdef VP_GCC
    __asm__ volatile("mov edi, %[dst]\n\t"
                     "mov esi, %[src]\n\t"
                     "mov ecx, %[n]\n\t"
                     "mov eax, ecx\n\t"
                     "shr ecx, 2\n\t"
                     "rep movsd\n\t"
                     "mov ecx, eax\n\t"
                     "and ecx, 3\n\t"
                     "rep movsb"
                     :
                     : [dst] "m"(dst), [src] "m"(src), [n] "m"(n)
                     : VP_X87_CLOBBERS, "eax", "ecx", "esi", "edi", "cc", "memory");
#else
    __asm { mov edi, dst
            mov esi, src
            mov ecx, n
            mov eax, ecx
            shr ecx, 2
            rep movsd
            mov ecx, eax
            and ecx, 3
            rep movsb }
#endif
}
static __forceinline void str_cat(char* dst, const char* src) { str_copy(dst + str_len(dst), src); }

// ---- the other groups' functions, by address -------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef uint8_t(__cdecl* Flag_t)();
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG_file = (AssertMsg_t)0x00411ae0;    // bare rets in this build
static const AssertMsg_t ASSERT_MSG_msg = (AssertMsg_t)0x00415e70;
static const AssertMsg_t ASSERT_MSG_shmem = (AssertMsg_t)0x004d8650;
typedef int(__cdecl* MultiBegin_t)(const char*);
static const MultiBegin_t MultiBegin = (MultiBegin_t)0x004150a0;
typedef void(__cdecl* Multi3_t)(int, const char*, int);
static const Multi3_t MultiEnter = (Multi3_t)0x00415180;            // _MultiEnter(handle, file, line)
static const Multi3_t MultiLeave = (Multi3_t)0x004151d0;
static const Multi3_t MultiEnd = (Multi3_t)0x00415220;
typedef void(__cdecl* ExitHandler_t)(Void_t);
static const ExitHandler_t ExceptInstallExitHandler = (ExitHandler_t)0x00415b90;
typedef const char*(__cdecl* Str_t)();
static const Str_t VersionGetBuildString = (Str_t)0x00410cb0;
static const Void_t abend = (Void_t)0x00416040;                     // sets a flag and writes to address 0
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* MemFree_t)(void*);
static const MemFree_t MemFree = (MemFree_t)0x00414300;
static const MemFree_t operator_delete = (MemFree_t)0x00414390;
typedef int(__cdecl* Int_t)();
static const Int_t TaskGetID = (Int_t)0x00414dd0;
typedef const char*(__cdecl* TaskGetName_t)(int);
static const TaskGetName_t TaskGetName = (TaskGetName_t)0x00414ec0;
typedef void*(__cdecl* SyncAlloc_t)();
static const SyncAlloc_t SyncMutexAlloc = (SyncAlloc_t)0x004152a0;
static const SyncAlloc_t SyncStrobeAlloc = (SyncAlloc_t)0x00415370;
typedef void(__cdecl* SyncPtr_t)(void*);
static const SyncPtr_t SyncMutexLock = (SyncPtr_t)0x00415330;
static const SyncPtr_t SyncMutexUnlock = (SyncPtr_t)0x00415340;
static const SyncPtr_t SyncStrobeFlash = (SyncPtr_t)0x004153b0;
static const SyncPtr_t SyncStrobeWait = (SyncPtr_t)0x00415380;
static const SyncPtr_t SyncLameMutexLock = (SyncPtr_t)0x004153e0;   // (int* lock)
static const SyncPtr_t SyncLameMutexUnlock = (SyncPtr_t)0x00415410;
typedef void(__cdecl* SyncFree_t)(void**);
static const SyncFree_t SyncMutexFree = (SyncFree_t)0x00415350;
static const SyncFree_t SyncStrobeFree = (SyncFree_t)0x004153c0;
typedef void(__cdecl* Key_t)(uint32_t);
static const Key_t KeyDown = (Key_t)0x00413e20;
static const Key_t KeyUp = (Key_t)0x00413e70;
typedef void(__cdecl* KeyChar_t)(uint32_t, uint32_t);                  // (unsigned, unsigned char): the dword as pushed
static const KeyChar_t KeyQueueChar = (KeyChar_t)0x00413dd0;
static const KeyChar_t KeyQueueMetaChar = (KeyChar_t)0x00413df0;
typedef void(__cdecl* MouseQueue_t)(int, int, int, int);
static const MouseQueue_t MouseQueueEvent = (MouseQueue_t)0x00414530;
typedef uint8_t(__cdecl* CreateWindow_t)(void*);
static const CreateWindow_t create_window = (CreateWindow_t)0x00412690;   // M2's SDL window when it's on
static const Void_t hook_keys = (Void_t)0x00412ac0;
static const Void_t Win32Idle = (Void_t)0x00412bf0;
static const Void_t log_devices = (Void_t)0x00412b20;                   // a bare ret

// ---- this group's own functions, by address (the hooked rewrite -- or the original -- is what runs) ---------------------
typedef void(__cdecl* LogFn_t)(const char*);
static const LogFn_t log_o = (LogFn_t)0x00411010;
static const Flag_t log_file_begin_o = (Flag_t)0x00410ef0;
static const Void_t mono_clear_o = (Void_t)0x00411500;
typedef void(__cdecl* Mputs_t)(int, int, const char*);
static const Mputs_t mputs_o = (Mputs_t)0x00411470;
typedef void(__cdecl* LogPanicV_t)(const char*, const char**, char*);
static const LogPanicV_t LogPanicV_o = (LogPanicV_t)0x004112d0;         // _LogPanic
typedef int(__cdecl* FileName_t)(const char*);
static const FileName_t FileCreate_o = (FileName_t)0x004115f0;
static const FileName_t FileOpen_o = (FileName_t)0x00411780;
typedef int(__cdecl* AllocFile_t)(const char*, HANDLE);
static const AllocFile_t alloc_file_o = (AllocFile_t)0x00411630;
typedef void(__cdecl* FileClose_t)(int*);
static const FileClose_t FileClose_o = (FileClose_t)0x00411850;
typedef int(__cdecl* FileSize_t)(int);
static const FileSize_t FileSize_o = (FileSize_t)0x00411890;
typedef uint8_t(__cdecl* FileReadExact_t)(int, void*, int);
static const FileReadExact_t FileReadExact_o = (FileReadExact_t)0x004118b0;
typedef uint8_t(__cdecl* FileRead_t)(int, void*, int*);
static const FileRead_t FileRead_o = (FileRead_t)0x00411940;
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
static const FileWrite_t FileWrite_o = (FileWrite_t)0x00411a30;
typedef void(__cdecl* FileFlush_t)(int);
static const FileFlush_t FileFlush_o = (FileFlush_t)0x00411af0;
typedef uint8_t(__cdecl* FileDir_t)(const char*);
static const FileDir_t FileCreateDirectory_o = (FileDir_t)0x00411d00;
static const FileDir_t FileCreateDirectoryRecursively_o = (FileDir_t)0x00411d30;
typedef uint8_t(__cdecl* GetSubDir_t)(char*, const char*, int);
static const GetSubDir_t get_sub_dir_o = (GetSubDir_t)0x00411d90;
typedef uint8_t(__cdecl* MsgBegin_t)(const char*, const char*);
static const MsgBegin_t MsgClientBegin_o = (MsgBegin_t)0x00415cf0;
typedef uint8_t(__cdecl* MsgSend_t)(const char*);
static const MsgSend_t MsgClientSend_o = (MsgSend_t)0x00415df0;
static const Void_t MsgClientEnd_o = (Void_t)0x00415e80;
static const Void_t unhook_keys_o = (Void_t)0x00412ba0;
static const Void_t destroy_window_o = (Void_t)0x00412b70;
typedef void(__cdecl* Restrict_t)(uint8_t);
static const Restrict_t restrict_cursor_o = (Restrict_t)0x00412ae0;
typedef const char*(__cdecl* ErrStr_t)(int);
static const ErrStr_t Win32GetErrorString_int_o = (ErrStr_t)0x00416080;
static const Str_t Win32GetErrorString_o = (Str_t)0x00416070;

// ---- layouts ----------------------------------------------------------------------------------------------------------
namespace {
struct FileEntry { char name[0x100]; HANDLE handle; };          // the file table's slot (0x104)
static_assert(sizeof(FileEntry) == 0x104, "FileEntry");
// FileMemoryMap (12): the view, the mapping (0 fake, -1 destroyed), fake (the file read into MemAlloc'd memory).
// Returned through a hidden pointer, copied as three dwords from a local: the pad bytes are whatever that local's
// stack held (not reproducible; LogPanic never returns where one is left unset).
struct FileMemoryMap { void* mem; HANDLE map; uint8_t fake, _pad[3]; };
static_assert(sizeof(FileMemoryMap) == 12, "FileMemoryMap");
struct MsgPacket { int32_t seq; char text[0x100]; };         // msg.obj's 0x508d38, 0x104 bytes on the wire
// shmem_block (0x40): three buffers {state, mem} (state 1 fresh / 0 written / 2 being read) cycled between the
// writer (wr), the latest complete one (ro) and a spare; the reader grabs ro
struct ShmemSlot { int32_t state; void* mem; };
struct ShmemBlock {
    uint32_t index;          // +0x00 its slot in the block table (handle = index + 1)
    const char* name;        // +0x04 (the caller's string, kept)
    void* mutex;             // +0x08 SyncMutexAlloc
    void* strobe;            // +0x0c SyncStrobeAlloc (flashed when a buffer is written)
    ShmemSlot* wr;           // +0x10 the writer's buffer
    ShmemSlot* spare;        // +0x14
    ShmemSlot* ro;           // +0x18 the latest written buffer
    ShmemSlot a, b, c;       // +0x1c, +0x24, +0x2c
    ShmemSlot* reading;      // +0x34 the buffer the reader holds (0 none)
    int32_t reader;          // +0x38 the reading task (ShmemGet's caller)
    int32_t writer;          // +0x3c the task that made it
};
static_assert(sizeof(ShmemBlock) == 0x40 && offsetof(ShmemBlock, reading) == 0x34, "ShmemBlock");
typedef uint8_t(__cdecl* MsgHook_t)(uint32_t, int, int);
}  // namespace

// ---- statics ----------------------------------------------------------------------------------------------------------
// log.obj
#define g_mono_off       (*(volatile uint8_t*)0x004e5a1c)
#define g_log_fn         (*(LogFn_t volatile*)0x004e5a20)
#define g_log_end        (*(Void_t volatile*)0x004e5a24)
#define g_log_bytes      (*(volatile int32_t*)0x004e5a28)
#define g_log_full       (*(volatile uint8_t*)0x004e5a2c)
#define g_mono_x         (*(volatile int32_t*)0x004e5a30)
#define g_mono_y         (*(volatile int32_t*)0x004e5a34)
#define g_log_multi      (*(volatile int32_t*)0x00505e58)
#define g_mono_line      (*(volatile int32_t*)0x00505e5c)
#define g_log_file       (*(volatile int32_t*)0x00505e60)
#define g_log_nhooks     (*(volatile int32_t*)0x00505e68)
#define g_log_hooks      ((LogFn_t volatile*)0x00505e6c)      // [10]
#define g_mono_multi     (*(volatile int32_t*)0x00505e98)
#define k_banner         (*(const char* const volatile*)0x004db430)   // -> "====..."
#define k_bang           (*(const char* const volatile*)0x004db434)   // -> "!!!!..."
// file.obj
#define g_file_multi     (*(volatile int32_t*)0x004e5c54)
#define g_temp_count     (*(volatile uint32_t*)0x004e5c58)
#define g_nfiles         (*(volatile int32_t*)0x00505ea0)
// The slots: 32 at 0x505ea8 -- M1 (viperport.cpp, relocate_res_tables) repoints every instruction that addresses them
// at 256 in the DLL (res_table_fields.inc), so each rewrite reads its own instruction's operand (docs/PORTING.md 12):
// file_at(m1_operand(op), i) for an operand that addresses the names (0x505ea8), FH(m1_operand(op), i) for one that
// addresses the handles (0x505fa8).
static FileEntry* file_at(uint32_t names, int i) { return (FileEntry*)(uintptr_t)(names + (uint32_t)i * 0x104); }
#define FH(handles, i)   (*(HANDLE volatile*)(uintptr_t)((handles) + (uint32_t)(i) * 0x104))
// win32.obj
#define g_hwnd           (*(HWND volatile*)0x004e5eac)
#define g_prev_fg        (*(HWND volatile*)0x004e5eb0)
#define g_instance       (*(HINSTANCE volatile*)0x004e5eb4)
#define g_args           (*(char* volatile*)0x004e5eb8)
#define g_key_hook       (*(HHOOK volatile*)0x004e5ebc)
#define g_inactive       (*(volatile uint8_t*)0x004e5ec0)
#define g_semaphore      (*(HANDLE volatile*)0x004e5ec4)
#define g_console        (*(volatile uint8_t*)0x004e5ec8)
#define g_loading_wnd    (*(HWND volatile*)0x004e5ecc)
#define g_splash_bmp     (*(HBITMAP volatile*)0x00507f30)
#define g_user_dir       ((char*)0x00507f48)                  // [0x104]
#define g_msg_hooks      ((MsgHook_t volatile*)0x00508060)     // [2]
#define k_sem_name       (*(const char* const volatile*)0x004db438)   // -> "MGI Viper Racing 1998"
#define k_class_name     (*(const char* const volatile*)0x004db43c)   // -> "Viper Racing Window"
#define k_app_name       (*(const char* const volatile*)0x004db440)   // -> "Viper Racing"
// msg.obj
#define g_msg            ((MsgPacket*)0x00508d38)
#define g_msg_seq        (*(volatile int32_t*)0x00508d38)
#define g_mailslot       (*(HANDLE volatile*)0x00508e3c)
// shmem.obj
#define g_shm_count      (*(volatile uint32_t*)0x00503a74)
#define g_shm_blocks     ((ShmemBlock* volatile*)0x005d5768)   // [9] (unbounded)
#define g_shm_mutex      ((void*)0x005d578c)
#define g_mono_screen    ((volatile uint16_t*)0x000b0000)      // the monochrome adapter's text memory

// the strings, at their game addresses (the original passes these pointers on)
#define S(a) ((const char*)(uintptr_t)(a))

// =======================================================================================================================
// log.obj
// =======================================================================================================================

// LogBegin (0x410d10): the hooks cleared; NT (no mono); the "Log" and "Mono" locks; log.cfg's first word, if any,
// names a machine whose mailslot \\<word>\mailslot\mgi\logger takes the log; else c:\log.log; else nothing. Then
// the banner and the build. log.cfg is opened and never closed (its slot stays taken). Its first word is found
// among the bytes read minus one: a word running to the last byte read loses that byte (a file without a trailing
// newline). The 32 bytes read aren't terminated (the buffer is zeroed first; 32 non-blank bytes run on).
static uint8_t __cdecl LogBegin_rw() {
    memset((void*)0x00505e68, 0, 0xb * 4);                         // the hook count and the 10 hooks
    DWORD v = kGetVersion();
    g_mono_off = v < 0x80000000u ? 1 : 0;
    g_log_multi = MultiBegin(S(0x004e5ac8));                       // "Log"
    g_mono_multi = MultiBegin(S(0x004e5acc));                      // "Mono"
    char cfg[0x20];
    cfg[0] = *S(0x004e5ad4);                                       // ""
    memset(cfg + 1, 0, 0x1f);
    int count;
    const int f = FileOpen_o(S(0x004e5ad8));                       // "log.cfg"
    if (f) {
        count = 0x20;
        if (FileRead_o(f, cfg, &count)) {
            char* s = cfg;
            count--;
            if (isspace_o((signed char)*s))
                for (;;) {
                    if ((uintptr_t)cfg + count <= (uintptr_t)s) break;
                    const char c = s[1];
                    s++;
                    if (!isspace_o((signed char)c)) break;
                }
            char* e = s;
            if (!isspace_o((signed char)*s))
                for (;;) {
                    if ((uintptr_t)cfg + count <= (uintptr_t)e) break;
                    const char c = e[1];
                    e++;
                    if (isspace_o((signed char)c)) break;
                }
            *e = 0;
            if (s != cfg) memmove_o(cfg, s, str_len(s) + 1);
        }
    }
    if (cfg[0] && MsgClientBegin_o(S(0x004e5ae0), cfg)) {          // "logger"
        g_log_fn = (LogFn_t)0x00410ff0;                             // msg_log
        g_log_end = (Void_t)0x00411000;                             // msg_end
    } else if (log_file_begin_o()) {
        g_log_fn = (LogFn_t)0x00410f10;                             // file_log
        g_log_end = (Void_t)0x00410fc0;                             // file_end
        ExceptInstallExitHandler((Void_t)0x00410fc0);
    } else {
        g_log_fn = (LogFn_t)0x00410cf0;                             // noop_log
        g_log_end = (Void_t)0x00410d00;                             // noop_end
    }
    const char* banner = k_banner;
    log_o(banner);
    log_o(VersionGetBuildString());
    log_o(banner);
    if (!g_mono_off) mono_clear_o();
    return 1;
}
static void fp_log_begin(Footprint& f) { f.replay_only = "it opens log.cfg and the log (a file or a mailslot) and makes the log's locks"; }
PORT_FN(0x00410d10, "LogBegin", LogBegin_rw, fp_log_begin)

// log_file_begin (0x410ef0): c:\log.log, created (the root of C: -- not writable without elevation today)
// FIX: the root of C: isn't writable without elevation, so there was no log at all. The log is
// <race.exe's folder>\log\log.log instead, the folder made (vrmod's patched "log\log.log", run from the game's
// folder, is the same file). A folder too long for the file table's 0x100-byte names keeps the literal. The path
// used is kept for FileVerifyNoOpenFiles, which must skip the log (the original compares a second literal).
static char s_log_path[0x100];                                     // the fixed log's path ("": the literal's)
static uint8_t __cdecl log_file_begin_rw() {
    const char* name = S(0x004e5ae8);                              // "c:\log.log"
    if (VP_FIX) {
        if (vp_log_path(s_log_path, sizeof s_log_path, "log.log")) {
            name = s_log_path;
            logf("fix: the game's log is %s", name);
        } else {
            s_log_path[0] = 0;
            logf("fix: race.exe's folder isn't known, or is too long for the log's path; the game's log stays at %s", name);
        }
    }
    const int f = FileCreate_o(name);
    g_log_file = f;
    return f != 0;
}
static void fp_log_file_begin(Footprint& f) { f.replay_only = "it creates the log file"; }
PORT_FN(0x00410ef0, "log_file_begin", log_file_begin_rw, fp_log_file_begin)

// file_log (0x410f10): each message on a new line ("\r\n" first) unless it starts with '$', which is dropped --
// but the length isn't, so a '$' message writes its terminating NUL into the log. Up to 1,000,000 bytes, then
// one "<<< Maximum logfile size reached >>>" (without a line break) and nothing more; flushed every time.
static void __cdecl file_log_rw(const char* msg) {
    if (g_log_bytes < 1000000) {
        const char* s = msg;
        const int n = (int)str_len(msg);
        if (*s != '$') FileWrite_o(g_log_file, S(0x004e5af4), 2);   // "\r\n"
        else s++;
        FileWrite_o(g_log_file, s, n);
        g_log_bytes += n;
    } else if (!g_log_full) {
        char m[0x28];
        g_log_full = 1;
        memcpy(m, S(0x004e5af8), 0x25);                              // "<<< Maximum logfile size reached >>>"
        FileWrite_o(g_log_file, m, 0x24);
    }
    FileFlush_o(g_log_file);
}
static void fp_file_log(Footprint& f, const char*) { f.replay_only = "it writes the log file"; }
PORT_FN(0x00410f10, "file_log", file_log_rw, fp_file_log)

// file_end (0x410fc0): the log file closed, the sinks back to the no-ops (also the exit handler)
static void __cdecl file_end_rw() {
    FileClose_o((int*)0x00505e60);
    g_log_file = 0;
    g_log_fn = (LogFn_t)0x00410cf0;
    g_log_end = (Void_t)0x00410d00;
}
static void fp_file_end(Footprint& f) { f.replay_only = "it closes the log file"; }
PORT_FN(0x00410fc0, "file_end", file_end_rw, fp_file_end)

// msg_log (0x410ff0) / msg_end (0x411000): the mailslot sink
static void __cdecl msg_log_rw(const char* msg) { MsgClientSend_o(msg); }
static void fp_msg_log(Footprint& f, const char*) { f.replay_only = "it writes to the log's mailslot"; }
PORT_FN(0x00410ff0, "msg_log", msg_log_rw, fp_msg_log)
static void __cdecl msg_end_rw() { MsgClientEnd_o(); }
static void fp_msg_end(Footprint& f) { f.replay_only = "it closes the log's mailslot"; }
PORT_FN(0x00411000, "msg_end", msg_end_rw, fp_msg_end)

// log (0x411010): the sink, each hook (the count re-read each time), and the mono monitor's next line (0..24)
static void __cdecl log_rw(const char* msg) {
    g_log_fn(msg);
    for (int i = 0; g_log_nhooks > i; i++) g_log_hooks[i](msg);
    const int line = g_mono_line;
    g_mono_line = g_mono_line + 1;
    mputs_o(0, line, msg);
    if (g_mono_line == 0x19) g_mono_line = 0;
}
static void fp_log(Footprint& f, const char*) { f.replay_only = "it runs the log's sink and hooks (I/O)"; }
PORT_FN(0x00411010, "log", log_rw, fp_log)

// LogEnd (0x411080): both locks ended and cleared, then the end sink (not reset here: file_end does)
static void __cdecl LogEnd_rw() {
    MultiEnd(g_log_multi, 0, 0);
    MultiEnd(g_mono_multi, 0, 0);
    g_mono_multi = 0;
    g_log_multi = 0;
    g_log_end();
}
static void fp_log_end(Footprint& f) { f.replay_only = "it ends the log's locks and closes the log"; }
PORT_FN(0x00411080, "LogEnd", LogEnd_rw, fp_log_end)

// LogUninstallHook (0x4110c0): the last hook moves into the removed one's place; not found panics
static void __cdecl LogUninstallHook_rw(void* hook) {     // (a LogFn_t; void* for test/fuzz.h)
    const LogFn_t fn = (LogFn_t)hook;
    if (g_log_nhooks > 0)
        for (int i = 0; g_log_nhooks > i; i++)
            if (g_log_hooks[i] == fn) {
                g_log_hooks[i] = g_log_hooks[g_log_nhooks - 1];
                g_log_nhooks = g_log_nhooks - 1;
                return;
            }
    LogPanic(S(0x004e5b20), g_log_nhooks, hook);                     // "LogUninstallHook: 0 of %d funcs matches 0x%x"
}
static void fp_log_hooks(Footprint& f, void*) { f.add((void*)0x00505e68, 0x2c, "log hooks"); }
PORT_FN(0x004110c0, "LogUninstallHook", LogUninstallHook_rw, fp_log_hooks)

// LogInstallHook (0x411120): up to 10 (compared unsigned)
static void __cdecl LogInstallHook_rw(void* hook) {       // (a LogFn_t)
    const LogFn_t fn = (LogFn_t)hook;
    if ((uint32_t)g_log_nhooks >= 10) {
        LogPanic(S(0x004e5b50));                                   // "LogInstallHook: Increase hook capacity"
        return;
    }
    g_log_hooks[g_log_nhooks] = fn;
    g_log_nhooks = g_log_nhooks + 1;
}
PORT_FN(0x00411120, "LogInstallHook", LogInstallHook_rw, fp_log_hooks)

// LogReport (0x411150): formatted into 0x100 bytes on the stack (vsprintf: unbounded), too long panics -- after the
// overrun (a message of 0x100 or more characters writes past the buffer: its terminator lands on the return
// address, longer ones on the caller's frame; LogPanic never returns)
static void __cdecl LogReport_rw(const char* fmt, Va va) {
    char buf[0x100];
    buf[0xff] = 0;
    vsprintf_o(buf, fmt, VA(va));
    if (buf[0xff] != 0) {
        LogPanic(S(0x004e5b78));                                   // "Someone LogReport'd a message that was too long!"
        return;
    }
    MultiEnter(g_log_multi, 0, 0);
    log_o(buf);
    MultiLeave(g_log_multi, 0, 0);
}
static void fp_log_report(Footprint& f, const char*, Va) { f.replay_only = "it writes the log"; }
PORT_FN(0x00411150, "LogReport", LogReport_rw, fp_log_report)

// LogError (0x4111d0): "ERROR : " and the message between two banners
static void __cdecl LogError_rw(const char* fmt, Va va) {
    char buf[0x100];
    memcpy(buf, S(0x004e5bac), 9);                                 // "ERROR : "
    memset(buf + 9, 0, 0xf7);
    buf[0xff] = 0;
    vsprintf_o(buf + str_len(buf), fmt, VA(va));
    if (buf[0xff] != 0) {
        LogPanic(S(0x004e5bb8));                                   // "Someone LogReport'd a message that was too long!"
        return;
    }
    MultiEnter(g_log_multi, 0, 0);
    const char* banner = k_banner;
    log_o(banner);
    log_o(buf);
    log_o(banner);
    MultiLeave(g_log_multi, 0, 0);
}
PORT_FN(0x004111d0, "LogError", LogError_rw, fp_log_report)

// _LogPanic (0x4112d0): prefix + message; if it fits, to the log (both locks made), else to the mono monitor
// (Win9x), else a message box; then abend. Too long: nothing is shown at all (after the overrun, as LogReport).
static void __cdecl LogPanicV_rw(const char* prefix, const char** fmt, char* args) {
    char buf[0x100];
    str_copy(buf, prefix);
    buf[0xff] = 0;
    vsprintf_o(buf + str_len(buf), *fmt, (va_list)args);
    if (buf[0xff] == 0) {
        if (g_log_multi != 0 && g_mono_multi != 0) {
            MultiEnter(g_log_multi, 0, 0);
            const char* banner = k_banner;
            log_o(banner);
            log_o(buf);
            log_o(banner);
            MultiLeave(g_log_multi, 0, 0);
        } else if (!g_mono_off) {
            const char* bang = k_bang;
            mputs_o(0, 0, bang);
            mputs_o(0, 1, S(0x004e5bf8));                          // "<before log module begins>"
            mputs_o(0, 2, buf);
            mputs_o(0, 0, bang);
        } else {
            kMessageBoxA(0, buf, S(0x004e5c14), 0x30);              // "Last-Resort Log", MB_ICONWARNING
        }
    }
    abend();
}
static void fp_log_panic_v(Footprint& f, const char*, const char**, char*) { f.replay_only = "it ends the game"; }
PORT_FN(0x004112d0, "_LogPanic", LogPanicV_rw, fp_log_panic_v)

// LogPanic (0x4112b0), LogHang (0x411410), LogAssert (0x411430), LogAssertMsg (0x411450): _LogPanic with a prefix
static void __cdecl LogPanic_rw(const char* fmt, Va va) { LogPanicV_o(S(0x004e5bec), &fmt, (char*)&va); }   // "Panic : "
static void __cdecl LogHang_rw(const char* fmt, Va va) { LogPanicV_o(S(0x004e5c24), &fmt, (char*)&va); }    // "Hang! "
static void __cdecl LogAssert_rw(const char* fmt, Va va) { LogPanicV_o(S(0x004e5c2c), &fmt, (char*)&va); }  // "ASSERTION FAILURE: "
static void __cdecl LogAssertMsg_rw(int cond, const char* fmt, Va va) {
    if (cond == 0) LogPanicV_o(S(0x004e5c40), &fmt, (char*)&va);  // "ASSERTION FAILURE: "
}
static void fp_log_panic(Footprint& f, const char*, Va) { f.replay_only = "it ends the game"; }
static void fp_log_assert_msg(Footprint& f, int cond, const char*, Va) {
    if (cond == 0) f.replay_only = "it ends the game";              // (holds: writes nothing)
}
PORT_FN(0x004112b0, "LogPanic", LogPanic_rw, fp_log_panic)
PORT_FN(0x00411410, "LogHang", LogHang_rw, fp_log_panic)
PORT_FN(0x00411430, "LogAssert", LogAssert_rw, fp_log_panic)
PORT_FN(0x00411450, "LogAssertMsg", LogAssertMsg_rw, fp_log_assert_msg)

// mputs (0x411470): a string at (x, y) of the monochrome adapter's text memory (0xb0000, 80x25 words), Win9x only.
// \7 then a byte sets the attribute (a \7 as the last character reads past the terminator); \t writes blanks
// (character bytes only) while the AND of 7 with every address it wrote stays nonzero; \n skips a cell; nothing
// stops at the screen's end.
static void __cdecl mputs_rw(int x, int y, const char* s) {
    if (g_mono_off) return;
    if (y < 0 || y > 0x18 || x < 0 || x > 0x4f) return;
    uint32_t p = 0x000b0000 + (uint32_t)((y * 80 + x) * 2);        // ebx
    uint32_t ax = 0x700;                                           // the character and its attribute
    const char* d = s;                                             // edx
    for (;; p += 2, d++) {
        const uint8_t c = (uint8_t)*d;
        ax = (ax & 0xff00) | c;
        if (c == 7) {
            d++;
            p -= 2;
            ax = (ax & 0xff) | ((uint32_t)(uint8_t)*d << 8);
        } else if (c == 0) {
            return;
        } else if (c == 9) {
            uint32_t m = 7;
            for (;;) {
                *(volatile uint8_t*)p = ' ';
                m &= p;
                if (m == 0) break;
                p += 2;
            }
        } else if (c == 10) {
        } else {
            *(volatile uint16_t*)p = (uint16_t)ax;
        }
    }
}
static void fp_mputs(Footprint& f, int, int, const char*) {
    if (!*(volatile uint8_t*)0x004e5a1c) f.replay_only = "it writes the mono monitor's memory at 0xb0000 (Win9x)";
}
PORT_FN(0x00411470, "mputs", mputs_rw, fp_mputs)

// mono_clear (0x411500): the cursor home, the screen blank (attribute 7)
static void __cdecl mono_clear_rw() {
    g_mono_y = 0;
    g_mono_x = 0;
    for (int i = 0; i < 0x3e8; i++) ((volatile uint32_t*)0x000b0000)[i] = 0x07000700;
}
static void fp_mono_clear(Footprint& f) { f.replay_only = "it writes the mono monitor's memory at 0xb0000 (Win9x)"; }
PORT_FN(0x00411500, "mono_clear", mono_clear_rw, fp_mono_clear)

// =======================================================================================================================
// file.obj
// =======================================================================================================================

// FileBegin (0x411540): the "File" lock; every slot free (the used count isn't reset)
static void __cdecl FileBegin_rw() {
    g_file_multi = MultiBegin(S(0x004e5c5c));                      // "File"
    // mov eax, table.handle ... cmp eax, <the handle after the last> (M1 moves both)
    for (uint32_t h = m1_operand(0x00411555); h < m1_operand(0x00411565); h += 0x104) {
        *(HANDLE volatile*)(uintptr_t)h = INVALID_HANDLE_VALUE;
        *(volatile char*)(uintptr_t)(h - 0x100) = 0;               // the slot's name
    }
}
static void fp_file_begin(Footprint& f) { f.replay_only = "it makes the File lock"; }
PORT_FN(0x00411540, "FileBegin", FileBegin_rw, fp_file_begin)

// FileVerifyNoOpenFiles (0x411580): each open slot but the log file's reported
static void __cdecl FileVerifyNoOpenFiles_rw() {
    // FIX: the log file is skipped by the path log_file_begin gave it (see there)
    const char* log_name = VP_FIX && s_log_path[0] ? (const char*)s_log_path : S(0x004e5c64);   // "c:\log.log"
    // mov esi, table ... cmp esi, <the name after the last> (M1 moves both). The first is in the bytes the hook
    // overwrites: once hooked, FileBegin's `mov eax, table.handle` (less the name's 0x100) says where the table is.
    const uint32_t table = m1_operand_hooked(0x00411583, 0x00411580, 0x56, 0x00411555, -0x100);   // (0x56: push esi)
    for (uint32_t a = table; a < m1_operand(0x004115bc); a += 0x104) {
        const FileEntry* e = (const FileEntry*)(uintptr_t)a;
        if (*(HANDLE volatile*)&e->handle != INVALID_HANDLE_VALUE && stricmp_o(log_name, e->name) != 0)
            LogReport(S(0x004e5c70), e->name);                     // "Handle to %s was never freed."
    }
}
static void fp_file_verify(Footprint& f) { f.replay_only = "it writes the log"; }
PORT_FN(0x00411580, "FileVerifyNoOpenFiles", FileVerifyNoOpenFiles_rw, fp_file_verify)

// FileEnd (0x4115d0)
static void __cdecl FileEnd_rw() { MultiEnd(g_file_multi, 0, 0); }
static void fp_file_end_lock(Footprint& f) { f.replay_only = "it ends the File lock"; }
PORT_FN(0x004115d0, "FileEnd", FileEnd_rw, fp_file_end_lock)

// FileCreate (0x4115f0): write-only, others may read, always a new file; the template handle is -1
static int __cdecl FileCreate_rw(const char* name) {
    HANDLE h = kCreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return 0;
    return alloc_file_o(name, h) + 1;
}
static void fp_file_io_name(Footprint& f, const char*) { f.replay_only = "file I/O"; }
PORT_FN(0x004115f0, "FileCreate", FileCreate_rw, fp_file_io_name)

// alloc_file (0x411630): under the lock, the next never-used slot while fewer than 32 have been used, then the
// first free one; none panics. The name is copied unbounded into the slot's 0x100 bytes (a name of 0x100..0x103
// characters runs into the handle, which is written after it; longer ones into the next slot).
// The capacity (M1): the original's `cmp edx, 0x20` is an imm8 -- it can't say 256 -- so M1, moving the table to 256
// slots, makes it 0x7f: the original, if left original, hands out at most 127 never-used slots and then reuses free
// ones below that, all inside the 256-slot table. This rewrite uses the whole table: 256 once M1 has moved it (the
// imm8 no longer 0x20), else the stock 32. Both are safe, mixed too: every slot either hands out is inside the table.
static int32_t file_capacity() { return (uint8_t)m1_operand(0x00411652) == 0x20 ? 0x20 : VP_LIFT_FILES; }
static int __cdecl alloc_file_rw(const char* name, HANDLE h) {
    MultiEnter(g_file_multi, 0, 0);
    int slot = -1;
    const int n = g_nfiles;
    if (n < file_capacity()) {
        slot = n;
        g_nfiles = n + 1;
    } else {
        for (int i = 0; i < n; i++)
            if (FH(m1_operand(0x00411667), i) == INVALID_HANDLE_VALUE) { slot = i; break; }   // mov ecx, table.handle
    }
    if (slot == -1) {
        LogPanic(S(0x004e5c90), name);                             // "file.cpp: Can't alloc file \"%s\", increase MAX_FILES"
    } else {
        str_copy(file_at(m1_operand(0x004116ac), slot)->name, name);           // lea edi, [edx + table]
        FH(m1_operand(0x004116bf), slot) = h;                                  // mov [edx + table.handle], ecx
    }
    MultiLeave(g_file_multi, 0, 0);
    return slot;
}
static void fp_alloc_file(Footprint& f, const char*, HANDLE) {
    f.add((void*)0x00505ea0, 4, "file slots used");
    f.add(file_at(m1_operand(0x004116ac), 0), (uint32_t)file_capacity() * 0x104, "file table");   // (32 or, moved by M1, 256 slots)
}
PORT_FN(0x00411630, "alloc_file", alloc_file_rw, fp_alloc_file)

// FileCreateTemp (0x4116f0): GetTempFileName(".", "mgi", counter) -- the first call's counter is 0, for which
// Windows creates the file itself, so the CREATE_NEW that follows fails (and the empty file stays); temporary,
// deleted on close. (Nothing calls it in v1.0.)
static int __cdecl FileCreateTemp_rw() {
    char name[0x104];
    memcpy(name, S(0x004e5cc4), 8);                                // "foo.tmp"
    memset(name + 8, 0, 0xfc);
    kGetTempFileNameA(S(0x004e5cd0), S(0x004e5ccc), g_temp_count, name);   // ".", "mgi"
    HANDLE h = kCreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, INVALID_HANDLE_VALUE);
    g_temp_count = g_temp_count + 1;
    if (h == INVALID_HANDLE_VALUE) return 0;
    return alloc_file_o(name, h) + 1;
}
static void fp_file_io(Footprint& f) { f.replay_only = "file I/O"; }
PORT_FN(0x004116f0, "FileCreateTemp", FileCreateTemp_rw, fp_file_io)

// FileOpen (0x411780): read-only, read-only attribute, sequential
static int __cdecl FileOpen_rw(const char* name) {
    HANDLE h = kCreateFileA(name, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING,
                            FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    return alloc_file_o(name, h) + 1;
}
PORT_FN(0x00411780, "FileOpen", FileOpen_rw, fp_file_io_name)

// FileAppend (0x4117c0): write-only, opened or created, at its end
static int __cdecl FileAppend_rw(const char* name) {
    HANDLE h = kCreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return 0;
    kSetFilePointer(h, 0, 0, FILE_END);
    return alloc_file_o(name, h) + 1;
}
PORT_FN(0x004117c0, "FileAppend", FileAppend_rw, fp_file_io_name)

// FileOpenWritable (0x411810): read-write, existing, random access (nothing calls it in v1.0)
static int __cdecl FileOpenWritable_rw(const char* name) {
    HANDLE h = kCreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    return alloc_file_o(name, h) + 1;
}
PORT_FN(0x00411810, "FileOpenWritable", FileOpenWritable_rw, fp_file_io_name)

// FileClose (0x411850): the caller's File is decremented in place, its handle closed, the slot freed (its name
// kept), the File zeroed
static void __cdecl FileClose_rw(int* fp) {
    volatile int* f = fp;
    *f = *f - 1;
    kCloseHandle(FH(m1_operand(0x00411864), *f));
    FH(m1_operand(0x0041187b), *f) = INVALID_HANDLE_VALUE;
    *f = 0;
}
static void fp_file_close(Footprint& f, int*) { f.replay_only = "file I/O"; }
PORT_FN(0x00411850, "FileClose", FileClose_rw, fp_file_close)

// FileSize (0x411890)
static int __cdecl FileSize_rw(int f) { return (int)kGetFileSize(FH(m1_operand(0x004118a1), f - 1), 0); }
static void fp_file_size(Footprint& f, int) { f.replay_only = "file I/O"; }
PORT_FN(0x00411890, "FileSize", FileSize_rw, fp_file_size)

// FileReadExact (0x4118b0): exactly n bytes, or a panic
static uint8_t __cdecl FileReadExact_rw(int f, void* buf, int n) {
    const int i = f - 1;
    DWORD got;
    if (!kReadFile(FH(m1_operand(0x004118d0), i), buf, (DWORD)n, &got, 0)) {
        const DWORD e = kGetLastError();
        LogPanic(S(0x004e5cf4), file_at(m1_operand(0x00411918), i)->name, n, e);           // "ReadFile(\"%s\",%d) fails (0x%x)"
        return 0;
    }
    if ((int)got == n) return 1;
    LogPanic(S(0x004e5cd4), n, file_at(m1_operand(0x004118fb), i)->name);                  // "Can't read %d bytes from \"%s\""
    return 0;
}
static void fp_file_read_exact(Footprint& f, int, void*, int) { f.replay_only = "file I/O"; }
PORT_FN(0x004118b0, "FileReadExact", FileReadExact_rw, fp_file_read_exact)

// FileRead (0x411940): up to *n bytes, *n = what was read. Its panic prints the pointer n, not *n.
static uint8_t __cdecl FileRead_rw(int f, void* buf, int* n) {
    const int i = f - 1;
    DWORD got;
    if (!kReadFile(FH(m1_operand(0x0041195e), i), buf, (DWORD)*n, &got, 0)) {
        const DWORD e = kGetLastError();
        LogPanic(S(0x004e5d14), file_at(m1_operand(0x0041198d), i)->name, n, e);           // "ReadFile(\"%s\",%d) fails (0x%x)"
        return 0;
    }
    *n = (int)got;
    return 1;
}
static void fp_file_read(Footprint& f, int, void*, int*) { f.replay_only = "file I/O"; }
PORT_FN(0x00411940, "FileRead", FileRead_rw, fp_file_read)

// FileReadLine (0x4119b0): a byte at a time, up to n, to a '\n'; then the byte before the '\n' is cleared (the
// '\r' of a CRLF line -- of a bare LF line, its last character; of an empty first line, buf[-1]) and the byte
// after it. A full buffer or the end of the file returns 0 with the line unterminated.
// FIX: the byte before the '\n' is cleared only if it is a '\r' of this line: a bare LF line keeps its last
// character, and an empty line no longer writes before the buffer. A CRLF line is read exactly as before (the
// same bytes, return value and file position).
static uint8_t __cdecl FileReadLine_rw(int f, char* buf, int n) {
    int count = 1;
    char c = 0;
    int i = 0;
    char* p = buf;
    if (n > 0) {
        for (;;) {
            if (!FileRead_o(f, &c, &count)) break;
            if (count <= 0) break;
            if (c == '\n') goto line;
            *p++ = c;
            i++;
            if (!(i < n)) break;
        }
    }
    if (c != '\n') return 0;
line:
    if (VP_FIX && (p == buf || p[-1] != '\r')) {                   // FIX: (above) no '\r' to cut
        p[0] = 0;
        return 1;
    }
    p[-1] = 0;
    p[0] = 0;
    return 1;
}
static void fp_file_read_line(Footprint& f, int, char*, int) { f.replay_only = "file I/O"; }
PORT_FN(0x004119b0, "FileReadLine", FileReadLine_rw, fp_file_read_line)

// FileWrite (0x411a30): all n bytes, or a panic
static uint8_t __cdecl FileWrite_rw(int f, const void* data, int n) {
    const int i = f - 1;
    FileEntry* e = file_at(m1_operand(0x00411a4b), i);
    ASSERT_MSG_file(n > 0 ? 1 : 0, S(0x004e5d34), e->name);        // "Attempt to write 0 bytes to file %s"
    DWORD put;
    if (!kWriteFile(FH(m1_operand(0x00411a78), i), data, (DWORD)n, &put, 0)) {
        const DWORD err = kGetLastError();
        LogPanic(S(0x004e5d88), e->name, n, err);                  // "WriteFile(\"%s\",%d) fails (0x%x)"
        return 0;
    }
    if ((int)put == n) return 1;
    LogPanic(S(0x004e5d58), e->name, n, put);                      // "Tried to WriteFile(\"%s\",%d), but only wrote %d"
    return 0;
}
static void fp_file_write(Footprint& f, int, const void*, int) { f.replay_only = "file I/O"; }
PORT_FN(0x00411a30, "FileWrite", FileWrite_rw, fp_file_write)

// FileFlush (0x411af0): a duplicate of the handle made and closed (Win9x's way to commit a file)
static void __cdecl FileFlush_rw(int f) {
    HANDLE dup;
    HANDLE to = kGetCurrentProcess();
    HANDLE h = FH(m1_operand(0x00411b17), f - 1);
    HANDLE from = kGetCurrentProcess();
    if (kDuplicateHandle(from, h, to, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) kCloseHandle(dup);
}
static void fp_file_flush(Footprint& f, int) { f.replay_only = "file I/O"; }
PORT_FN(0x00411af0, "FileFlush", FileFlush_rw, fp_file_flush)

// FilePrintf (0x411b40): formatted into the caller's buffer (unbounded), written
static void __cdecl FilePrintf_rw(int f, char* buf, const char* fmt, Va va) {
    vsprintf_o(buf, fmt, VA(va));
    FileWrite_o(f, buf, (int)str_len(buf));
}
static void fp_file_printf(Footprint& f, int, char*, const char*, Va) { f.replay_only = "file I/O"; }
PORT_FN(0x00411b40, "FilePrintf", FilePrintf_rw, fp_file_printf)

// FileSetWritable (0x411b80): normal or read-only
static void __cdecl FileSetWritable_rw(const char* name, uint8_t writable) {
    kSetFileAttributesA(name, writable ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY);
}
static void fp_file_set_writable(Footprint& f, const char*, uint8_t) { f.replay_only = "file I/O"; }
PORT_FN(0x00411b80, "FileSetWritable", FileSetWritable_rw, fp_file_set_writable)

// FileRemove (0x411ba0)
static uint8_t __cdecl FileRemove_rw(const char* name) { return kDeleteFileA(name) != 0 ? 1 : 0; }
PORT_FN(0x00411ba0, "FileRemove", FileRemove_rw, fp_file_io_name)

// FileSeekAbsolute (0x411bc0) / FileSeekRelative (0x411bf0): not permitted -- a panic (nothing calls them)
static uint8_t __cdecl FileSeekAbsolute_rw(int f, int pos) {
    LogPanic(S(0x004e5da8), file_at(m1_operand(0x00411bd4), f - 1)->name, pos);   // "FileSeekAbsolute(\"%s\",%d): Seeking is not permitted."
    return 0;
}
static uint8_t __cdecl FileSeekRelative_rw(int f, int pos) {
    LogPanic(S(0x004e5de0), file_at(m1_operand(0x00411c04), f - 1)->name, pos);   // "FileSeekRelative(\"%s\",%d): Seeking is not permitted."
    return 0;
}
static void fp_file_seek(Footprint& f, int, int) { f.replay_only = "it panics (ends the game)"; }
PORT_FN(0x00411bc0, "FileSeekAbsolute", FileSeekAbsolute_rw, fp_file_seek)
PORT_FN(0x00411bf0, "FileSeekRelative", FileSeekRelative_rw, fp_file_seek)

// FileFindFirst (0x411c20) / FileFindNext (0x411c80): the name strncpy'd -- not terminated when it has n or more
// characters
static HANDLE __cdecl FileFindFirst_rw(const char* pattern, char* name, int n) {
    WIN32_FIND_DATAA fd;
    HANDLE h = kFindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    strncpy_o(name, fd.cFileName, (size_t)n);
    return h;
}
static void fp_file_find_first(Footprint& f, const char*, char*, int) { f.replay_only = "file I/O"; }
PORT_FN(0x00411c20, "FileFindFirst", FileFindFirst_rw, fp_file_find_first)
static uint8_t __cdecl FileFindNext_rw(HANDLE h, char* name, int n) {
    WIN32_FIND_DATAA fd;
    if (!kFindNextFileA(h, &fd)) return 0;
    strncpy_o(name, fd.cFileName, (size_t)n);
    return 1;
}
static void fp_file_find_next(Footprint& f, HANDLE, char*, int) { f.replay_only = "file I/O"; }
PORT_FN(0x00411c80, "FileFindNext", FileFindNext_rw, fp_file_find_next)
static void __cdecl FileFindClose_rw(HANDLE h) { kFindClose(h); }
static void fp_file_find_close(Footprint& f, HANDLE) { f.replay_only = "file I/O"; }
PORT_FN(0x00411cd0, "FileFindClose", FileFindClose_rw, fp_file_find_close)

// _FileChangeDir (0x411ce0): the CRT's chdir
static uint8_t __cdecl FileChangeDir_rw(const char* dir) { return chdir_o(dir) == 0 ? 1 : 0; }
PORT_FN(0x00411ce0, "_FileChangeDir", FileChangeDir_rw, fp_file_io_name)

// FileCreateDirectory (0x411d00): made, or already there
static uint8_t __cdecl FileCreateDirectory_rw(const char* dir) {
    if (kCreateDirectoryA(dir, 0)) return 1;
    return kGetLastError() == ERROR_ALREADY_EXISTS ? 1 : 0;
}
PORT_FN(0x00411d00, "FileCreateDirectory", FileCreateDirectory_rw, fp_file_io_name)

// FileCreateDirectoryRecursively (0x411d30): each prefix get_sub_dir gives, in 0x100 bytes on the stack (a longer
// prefix overruns it). Only components followed by a backslash are made: "a\b\c" makes a and a\b, not c.
static uint8_t __cdecl FileCreateDirectoryRecursively_rw(const char* path) {
    char sub[0x100];
    int i = 0;
    if (get_sub_dir_o(sub, path, 0)) {
        do {
            if (!FileCreateDirectory_o(sub)) return 0;
            i++;
        } while (get_sub_dir_o(sub, path, i));
    }
    return 1;
}
PORT_FN(0x00411d30, "FileCreateDirectoryRecursively", FileCreateDirectoryRecursively_rw, fp_file_io_name)

// get_sub_dir (0x411d90): the path up to (not including) its (n+1)th backslash -- skipping the one after "X:" and
// a UNC path's server and share -- strncpy'd and terminated: 1. No such backslash: 0 if exactly n were skipped,
// else the whole path strcpy'd and whether anything follows the last backslash.
static uint8_t __cdecl get_sub_dir_rw(char* out, const char* path, int n) {
    const char* s = path;
    int k;
    if (path[0] == '\\' && path[1] == '\\') {
        s = path + 2;
        k = n + 2;
    } else if (path[1] == ':') {
        k = n;
        if (path[2] == '\\') k++;
    } else {
        k = n;
    }
    for (;;) {
        const char* p = strchr_o(s, '\\');
        if (!p) break;
        if (k == 0) {
            const int len = (int)(p - path);
            strncpy_o(out, path, (size_t)len);
            out[len] = 0;
            return 1;
        }
        s = p + 1;
        k--;
    }
    if (k == 0) return 0;
    str_copy(out, path);
    return *s != 0 ? 1 : 0;
}
static void fp_get_sub_dir(Footprint& f, char* out, const char* path, int) {
    f.add(out, str_len(path) + 1, "sub-directory");
}
PORT_FN(0x00411d90, "get_sub_dir", get_sub_dir_rw, fp_get_sub_dir)

// FileCopy (0x411e40): the SECOND argument copied onto the first (overwriting); CopyFile's BOOL's low byte
static uint8_t __cdecl FileCopy_rw(const char* a, const char* b) { return (uint8_t)kCopyFileA(b, a, FALSE); }
static void fp_file_copy(Footprint& f, const char*, const char*) { f.replay_only = "file I/O"; }
PORT_FN(0x00411e40, "FileCopy", FileCopy_rw, fp_file_copy)

// create_fake_memory_map (0x411e60): the slot's file (index, not File) opened again by name, read whole into
// MemAlloc'd memory, closed
static FileMemoryMap* __cdecl create_fake_memory_map_rw(FileMemoryMap* ret, int slot) {
    FileMemoryMap m;                                               // (the pad and, on a panic, .map: stack contents)
    m.mem = 0;
    m.fake = 1;
    const char* name = file_at(m1_operand(0x00411e81), slot)->name;
    int f = FileOpen_o(name);
    if (f) {
        const int size = FileSize_o(f);
        void* mem = MemAlloc(size);
        FileReadExact_o(f, mem, size);
        FileClose_o(&f);
        m.map = 0;
        m.mem = mem;
        LogReport(S(0x004e5e28), name);                            // "Created fake memory map for %s"
    } else {
        LogPanic(S(0x004e5e48), name);                             // "create_fake_memory_map: Can't open %s!"
    }
    memcpy(ret, &m, 12);
    return ret;
}
static void fp_fake_map(Footprint& f, FileMemoryMap*, int) { f.replay_only = "it reads a file into allocated memory"; }
PORT_FN(0x00411e60, "create_fake_memory_map", create_fake_memory_map_rw, fp_fake_map)

// destroy_fake_memory_map (0x411f10)
static void __cdecl destroy_fake_memory_map_rw(FileMemoryMap* m) {
    operator_delete(m->mem);
    m->mem = 0;
}
static void fp_destroy_fake_map(Footprint& f, FileMemoryMap*) { f.replay_only = "it frees"; }
PORT_FN(0x00411f10, "destroy_fake_memory_map", destroy_fake_memory_map_rw, fp_destroy_fake_map)

// FileCreateMemoryMap (0x411f30): a read-only view of the whole file, or (logged) the fake one -- an empty file
// can't be mapped
typedef FileMemoryMap*(__cdecl* FakeMap_t)(FileMemoryMap*, int);
static const FakeMap_t create_fake_memory_map_o = (FakeMap_t)0x00411e60;
static FileMemoryMap* __cdecl FileCreateMemoryMap_rw(FileMemoryMap* ret, int f) {
    FileMemoryMap m;
    m.fake = 0;
    const int i = f - 1;
    HANDLE map = kCreateFileMappingA(FH(m1_operand(0x00411f53), i), 0, PAGE_READONLY, 0, 0, 0);
    if (!map) {
        LogReport(S(0x004e5e8c), Win32GetErrorString_o());         // "Can't CreateFileMapping! (%s)"
        create_fake_memory_map_o(ret, i);
        return ret;
    }
    void* view = kMapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    m.mem = view;
    if (!view) {
        kCloseHandle(map);
        LogReport(S(0x004e5e70), Win32GetErrorString_o());         // "Can't MapViewOfFile! (%s)"
        create_fake_memory_map_o(ret, i);
        return ret;
    }
    m.map = map;
    memcpy(ret, &m, 12);
    return ret;
}
static void fp_create_map(Footprint& f, FileMemoryMap*, int) { f.replay_only = "it maps a file"; }
PORT_FN(0x00411f30, "FileCreateMemoryMap", FileCreateMemoryMap_rw, fp_create_map)

// FileDestroyMemoryMap (0x412000)
typedef void(__cdecl* MapArg_t)(FileMemoryMap*);
static const MapArg_t destroy_fake_memory_map_o = (MapArg_t)0x00411f10;
static void __cdecl FileDestroyMemoryMap_rw(FileMemoryMap* m) {
    if (m->fake) {
        destroy_fake_memory_map_o(m);
        return;
    }
    kUnmapViewOfFile(m->mem);
    HANDLE map = *(HANDLE volatile*)&m->map;
    *(void* volatile*)&m->mem = 0;
    kCloseHandle(map);
    *(HANDLE volatile*)&m->map = INVALID_HANDLE_VALUE;
}
static void fp_destroy_map(Footprint& f, FileMemoryMap*) { f.replay_only = "it unmaps or frees"; }
PORT_FN(0x00412000, "FileDestroyMemoryMap", FileDestroyMemoryMap_rw, fp_destroy_map)

// =======================================================================================================================
// win32.obj
// =======================================================================================================================

// check_for_canary_launch (0x4123a0)
static uint8_t __cdecl check_for_canary_launch_rw() { return 1; }
static void fp_pure0(Footprint& f) { f.pure = true; }
PORT_FN(0x004123a0, "check_for_canary_launch", check_for_canary_launch_rw, fp_pure0)

// start_unique_instance (0x4123b0): a named semaphore; if it already exists another copy runs: its window (by
// class and title) restored, 0. A failed CreateSemaphore panics.
// TEST (viperport.ini [test] two_copies=1, README "Testing multiplayer on one PC"): a process holding one of the two
// copies' slots (viperport.cpp takes it at load) starts beside the running copy; the semaphore stays open, as for the
// first, and end_unique_instance closes it.
static uint8_t __cdecl start_unique_instance_rw() {
    HANDLE s = kCreateSemaphoreA(0, 0, 1, k_sem_name);
    g_semaphore = s;
    if (!s) LogPanic(S(0x004e5fb4));                               // "CreateSemaphore failed"
    if (kGetLastError() != ERROR_ALREADY_EXISTS) return 1;
    if (vp_g_two_copies) {
        logf("two_copies: copy %d starts beside the copy already running", vp_g_copy);
        return 1;
    }
    const char* title = k_app_name;
    const char* cls = k_class_name;
    HWND w = kFindWindowA(cls, title);
    if (w) kShowWindow(w, SW_RESTORE);
    kCloseHandle(g_semaphore);
    return 0;
}
static void fp_win_os(Footprint& f) { f.replay_only = "it calls the OS"; }
PORT_FN(0x004123b0, "start_unique_instance", start_unique_instance_rw, fp_win_os)

// end_unique_instance (0x412420)
static void __cdecl end_unique_instance_rw() { kCloseHandle(g_semaphore); }
PORT_FN(0x00412420, "end_unique_instance", end_unique_instance_rw, fp_win_os)

// ---- FIX: the user directory next to race.exe -------------------------------------------------------------------------
// The original's user directory is a literal: "C:\Program Files\MGI\Viper98\" on a stock exe -- wherever the game is
// installed, shared by every install on the machine, and not writable without elevation (UAC then puts what the game
// writes in %LOCALAPPDATA%\VirtualStore\Program Files\MGI\Viper98\) -- or vrmod's writepaths patch's relative
// "Config\", which follows the current directory. The fix makes it <race.exe's folder>\Config\, from
// GetModuleFileNameA(NULL). The first time (no such folder yet), the old user directory's whole tree is copied into
// it: the literal as this exe has it -- an absolute one's VirtualStore copies first (where a game without elevation
// really wrote), then the folder itself; a relative one from the current directory, unless that is the new folder.
// The second copy under viperport.ini [test] two_copies=1 has its own, <race.exe's folder>\Config-2\, seeded the same
// way but from <race.exe's folder>\Config\ first (the first copy's, already migrated), so the two never share a file.
// The old files are only read. Every user of the directory (options.cfg, the *.sco records, ghostcar\, setups\,
// paint*.tex, the career files, replays) appends a file name to Win32GetUserDirectory() in a MAX_PATH-sized buffer
// (FileCreateDirectoryRecursively's prefixes and the file table's names are 0x100 bytes), and WinMain logs "Long
// User Directory (%d), I am scared." past 200 characters: a longer path keeps the original's literal.
enum { FIX_USER_DIR_MAX = 0xc8 };

// is `dir` (with its trailing backslash) a directory?
static bool fix_is_dir(const char* dir) {
    char p[MAX_PATH];
    size_t n = strlen(dir);
    if (n < 2 || n >= MAX_PATH) return false;
    memcpy(p, dir, n + 1);
    if (p[n - 1] == '\\') p[n - 1] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = kFindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    kFindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// every file and folder under from\ copied into to\ (both with a trailing backslash; to\ exists): nothing is
// overwritten, the source only read. Links and folders nested past 16 levels are left out.
struct FixCopy { int files, failed, skipped; };
static void fix_copy_tree(const char* from, const char* to, FixCopy& st, int depth) {
    char a[MAX_PATH], b[MAX_PATH];
    const size_t nf = strlen(from), nt = strlen(to);
    if (nf + 2 > MAX_PATH) { st.skipped++; return; }
    memcpy(a, from, nf);
    memcpy(a + nf, "*", 2);
    WIN32_FIND_DATAA fd;
    HANDLE h = kFindFirstFileA(a, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* nm = fd.cFileName;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        const size_t nn = strlen(nm);
        const bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (nf + nn + 2 > MAX_PATH || nt + nn + 2 > MAX_PATH ||
            (dir && (depth >= 16 || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))) {
            st.skipped++;
            continue;
        }
        memcpy(a + nf, nm, nn + 1);
        memcpy(b, to, nt);
        memcpy(b + nt, nm, nn + 1);
        if (!dir) {
            if (kCopyFileA(a, b, TRUE)) st.files++;
            else st.failed++;
            continue;
        }
        if (!kCreateDirectoryA(b, 0) && kGetLastError() != ERROR_ALREADY_EXISTS) { st.failed++; continue; }
        memcpy(a + nf + nn, "\\", 2);
        memcpy(b + nt + nn, "\\", 2);
        fix_copy_tree(a, b, st, depth + 1);
    } while (kFindNextFileA(h, &fd));
    kFindClose(h);
}

// g_user_dir <- <race.exe's folder>\Config\, made, with the old user directory copied into it the first time.
// false (g_user_dir untouched, logged) where it can't: the original's literal is used.
static bool fix_user_directory() {
    const char* lit = S(0x004e5fcc);                               // the literal, as this exe has it
    char dir[MAX_PATH];
    const uint32_t n = vp_exe_dir(dir, sizeof dir);
    if (n == 0) {
        logf("fix: user directory: race.exe's folder not found; keeping %s", lit);
        return false;
    }
    const uint32_t ns = (uint32_t)strlen(vp_g_copy_suffix);        // "" ("-2": the second of two copies)
    if (n + 7 + ns > FIX_USER_DIR_MAX) {
        logf("fix: user directory: %sConfig%s\\ would be %u characters (the game takes %d); keeping %s", dir,
             vp_g_copy_suffix, n + 7 + ns, FIX_USER_DIR_MAX, lit);
        return false;
    }
    memcpy(dir + n, "Config", 6);
    memcpy(dir + n + 6, vp_g_copy_suffix, ns + 1);
    const BOOL made = kCreateDirectoryA(dir, 0);                   // (made now: a new user directory)
    const DWORD err = made ? 0 : kGetLastError();
    memcpy(dir + n + 6 + ns, "\\", 2);
    char note[3 * MAX_PATH];
    if (!made) {
        if (err == ERROR_ALREADY_EXISTS) snprintf(note, sizeof note, "already there");
        else snprintf(note, sizeof note, "can't be made: error %lu", (unsigned long)err);
    } else {
        // the old user directory: an absolute literal's VirtualStore copies (Program Files, then Program Files (x86)),
        // then the literal itself -- a relative one taken from the current directory
        char cand[4][MAX_PATH];
        int nc = 0;
        if (ns) {                                                  // the second copy: the first copy's Config\ first
            memcpy(cand[nc], dir, n);
            memcpy(cand[nc] + n, "Config\\", 8);
            nc++;
        }
        if (lit[0] && lit[1] == ':' && lit[2] == '\\') {
            char la[MAX_PATH];
            const DWORD k = GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la);   // (not a game import: the DLL's)
            if (k && k < sizeof la) {
                int w = snprintf(cand[nc], MAX_PATH, "%s\\VirtualStore\\%s", la, lit + 3);
                if (w > 0 && w < MAX_PATH) nc++;
                if (_strnicmp(lit + 3, "Program Files\\", 14) == 0) {
                    w = snprintf(cand[nc], MAX_PATH, "%s\\VirtualStore\\Program Files (x86)\\%s", la, lit + 17);
                    if (w > 0 && w < MAX_PATH) nc++;
                }
            }
        }
        const DWORD k = kGetFullPathNameA(lit, MAX_PATH, cand[nc], 0);
        if (k && k < MAX_PATH) nc++;
        const char* old = 0;
        for (int i = 0; i < nc && !old; i++)
            if (fix_is_dir(cand[i])) old = cand[i];
        if (!old) {
            snprintf(note, sizeof note, "new; no old user directory at %s", lit);
        } else {
            char from[MAX_PATH];
            const size_t no = strlen(old);
            memcpy(from, old, no + 1);
            if (no && from[no - 1] != '\\' && no + 1 < MAX_PATH) memcpy(from + no, "\\", 2);
            if (_strnicmp(dir, from, strlen(from)) == 0) {           // the same folder (or the new one inside it)
                snprintf(note, sizeof note, "new; the old user directory %s is this one", from);
            } else {
                FixCopy st = {0, 0, 0};
                fix_copy_tree(from, dir, st, 0);
                int w = snprintf(note, sizeof note, "copied %d files from %s", st.files, from);
                if (st.failed && w > 0 && w < (int)sizeof note)
                    w += snprintf(note + w, sizeof note - w, ", %d failed", st.failed);
                if (st.skipped && w > 0 && w < (int)sizeof note)
                    snprintf(note + w, sizeof note - w, ", %d left out", st.skipped);
            }
        }
    }
    logf("fix: user directory %s (%s)", dir, note);
    str_copy(g_user_dir, dir);
    return true;
}

// get_user_directory (0x412430): always "C:\Program Files\MGI\Viper98\" -- wherever the game is installed -- made
// (its result ignored) and logged
// FIX: <race.exe's folder>\Config\, the old one migrated (above)
typedef uint8_t(__cdecl* CreateDir_t)(const char*);
static const CreateDir_t create_directory_o = (CreateDir_t)0x00412480;
static uint8_t __cdecl get_user_directory_rw() {
    if (!(VP_FIX && fix_user_directory()))
        str_copy(g_user_dir, S(0x004e5fcc));                       // "C:\Program Files\MGI\Viper98\"
    create_directory_o(g_user_dir);
    LogReport(S(0x004e5fec), g_user_dir);                          // "Config Dir: %s"
    return 1;
}
static void fp_user_dir(Footprint& f) { f.replay_only = "it makes directories and writes the log"; }
PORT_FN(0x00412430, "get_user_directory", get_user_directory_rw, fp_user_dir)

// create_directory (0x412480)
static uint8_t __cdecl create_directory_rw(const char* dir) { return FileCreateDirectoryRecursively_o(dir); }
static void fp_create_dir(Footprint& f, const char*) { f.replay_only = "it makes directories"; }
PORT_FN(0x00412480, "create_directory", create_directory_rw, fp_create_dir)

// make_loading_window (0x412490): the 300x373 splash, centred, class "MGILoadingWindow" with splash_proc
static void __cdecl make_loading_window_rw() {
    const int cx = kGetSystemMetrics(SM_CXSCREEN);
    const int cy = kGetSystemMetrics(SM_CYSCREEN);
    WNDCLASSA wc;
    wc.style = 0;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = g_instance;
    wc.lpfnWndProc = (WNDPROC)0x00412570;                          // splash_proc
    wc.hIcon = 0;
    wc.hCursor = kLoadCursorA(0, MAKEINTRESOURCEA(0x7f00));        // IDC_ARROW
    wc.lpszMenuName = 0;
    wc.hbrBackground = (HBRUSH)6;
    wc.lpszClassName = S(0x004e5ffc);                              // "MGILoadingWindow"
    if (!kRegisterClassA(&wc)) return;
    HWND w = kCreateWindowExA(WS_EX_TOOLWINDOW, S(0x004e5ffc), S(0x004e6010), WS_POPUP | WS_BORDER,   // "MGI"
                              (cx - 300) / 2, (cy - 373) / 2, 300, 373, 0, 0, g_instance, 0);
    g_loading_wnd = w;
    if (!w) return;
    kShowWindow(w, SW_SHOW);
    kUpdateWindow(g_loading_wnd);
}
static void fp_win_window(Footprint& f) { f.replay_only = "it makes a window"; }
PORT_FN(0x00412490, "make_loading_window", make_loading_window_rw, fp_win_window)

// splash_proc (0x412570): the bitmap "SPLASH" loaded on WM_CREATE (none: -1, the window isn't made), deleted on
// WM_DESTROY, blitted on WM_PAINT (the memory DC's old bitmap never selected back); DefWindowProc after both
static LRESULT __stdcall splash_proc_rw(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        HBITMAP b = kLoadBitmapA(g_instance, S(0x004e6014));      // "SPLASH"
        g_splash_bmp = b;
        return b ? 0 : -1;
    }
    if (msg == WM_DESTROY) {
        kDeleteObject(g_splash_bmp);
    } else if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = kBeginPaint(hwnd, &ps);
        HDC mem = kCreateCompatibleDC(dc);
        kSelectObject(mem, g_splash_bmp);
        kBitBlt(dc, 0, 0, 300, 373, mem, 0, 0, SRCCOPY);
        kDeleteDC(mem);
        kEndPaint(hwnd, &ps);
    }
    return kDefWindowProcA(hwnd, msg, wp, lp);
}
static void fp_win_proc(Footprint& f, HWND, UINT, WPARAM, LPARAM) { f.replay_only = "a window procedure"; }
PORT_FN(0x00412570, "splash_proc", splash_proc_rw, fp_win_proc)

// Win32IsConsoleMode (0x412640) (nothing calls it)
static uint8_t __cdecl Win32IsConsoleMode_rw() { return g_console; }
static void fp_none(Footprint&) {}
PORT_FN(0x00412640, "Win32IsConsoleMode", Win32IsConsoleMode_rw, fp_none)

// Win32Begin (0x412650): unless in console mode, the window (create_window: M2's SDL one when that's on) and the
// keyboard hook
static void __cdecl Win32Begin_rw() {
    if (g_console) return;
    log_devices();
    if (create_window(g_instance)) {
        hook_keys();
        return;
    }
    LogPanic(S(0x004e601c));                                       // "unable to create out window!"
}
PORT_FN(0x00412650, "Win32Begin", Win32Begin_rw, fp_win_window)

// win32_event (0x412780): the original game window's procedure (dead under M2's SDL window). Keys and characters
// to the key queue, mouse messages to the mouse queue (x, y as unsigned 16 bits; buttons 1 left, 2 right, 4
// middle), activation to the inactive flag and the cursor clip, Alt / the task list / the screen saver / monitor
// power swallowed, the cursor hidden; everything else to the two message hooks and DefWindowProc.
static LRESULT __stdcall win32_event_rw(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        kShowCursor(1);
        restrict_cursor_o(0);
        kPostQuitMessage(0);
        break;
    case WM_ACTIVATEAPP: {
        const uint32_t a = (uint32_t)wp & 0xffff;
        if (a == 1 || a == 2) {
            g_inactive = 0;
            restrict_cursor_o(1);
        } else if (a == 0) {
            g_inactive = 1;
            restrict_cursor_o(0);
        }
        break;
    }
    case WM_SETCURSOR:
        kSetCursor(0);
        return 1;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        KeyDown((uint32_t)wp);
        KeyQueueMetaChar((uint32_t)wp, (uint32_t)((int32_t)lp >> 16));
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        KeyUp((uint32_t)wp);
        break;
    case WM_CHAR:
        KeyQueueChar((uint32_t)wp, (uint32_t)((int32_t)lp >> 16));
        break;
    case WM_SYSCOMMAND:
        if (wp == SC_KEYMENU || wp == SC_TASKLIST || wp == SC_SCREENSAVE || wp == SC_MONITORPOWER) return 0;
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        // (the original's own switch on the type also panics "Unrecognized mouse event type!" for the double
        // clicks, 0x203 and 0x206 -- which its outer switch never sends here: left out, rule 11)
        const int x = (int)((uint32_t)lp & 0xffff);
        const int y = (int)(((uint32_t)lp >> 16) & 0xffff);
        int state = 0;
        if (wp & MK_LBUTTON) state = 1;
        if (wp & MK_RBUTTON) state |= 2;
        if (wp & MK_MBUTTON) state |= 4;
        int type;
        switch (msg) {
        case WM_MOUSEMOVE: type = 6; break;
        case WM_LBUTTONDOWN: type = 0; break;
        case WM_LBUTTONUP: type = 1; break;
        case WM_RBUTTONDOWN: type = 2; break;
        case WM_RBUTTONUP: type = 3; break;
        case WM_MBUTTONDOWN: type = 4; break;
        default: type = 5; break;                                  // WM_MBUTTONUP
        }
        MouseQueueEvent(type, x, y, state);
        return 1;
    }
    default:
        break;
    }
    for (int i = 0; i < 2; i++) {
        MsgHook_t h = g_msg_hooks[i];
        if (h) h(msg, (int)wp, (int)lp);
    }
    return kDefWindowProcA(hwnd, msg, wp, lp);
}
PORT_FN(0x00412780, "win32_event", win32_event_rw, fp_win_proc)

// restrict_cursor (0x412ae0): the cursor clipped to the top-left 640x480, or freed
static void __cdecl restrict_cursor_rw(uint8_t on) {
    if (on) {
        RECT r;
        r.left = 0;
        r.top = 0;
        r.right = 0x280;
        r.bottom = 0x1e0;
        kClipCursor(&r);
    } else {
        kClipCursor(0);
    }
}
static void fp_restrict_cursor(Footprint& f, uint8_t) { f.replay_only = "it clips the cursor"; }
PORT_FN(0x00412ae0, "restrict_cursor", restrict_cursor_rw, fp_restrict_cursor)

// key_hook_proc (0x412b30): hook_keys' keyboard hook, passing everything on (dead under M2's SDL window)
static LRESULT __stdcall key_hook_proc_rw(int code, WPARAM wp, LPARAM lp) { return kCallNextHookEx(g_key_hook, code, wp, lp); }
static void fp_key_hook(Footprint& f, int, WPARAM, LPARAM) { f.replay_only = "a keyboard hook"; }
PORT_FN(0x00412b30, "key_hook_proc", key_hook_proc_rw, fp_key_hook)

// Win32End (0x412b50) / destroy_window (0x412b70) / unhook_keys (0x412ba0)
static void __cdecl Win32End_rw() {
    if (g_console) return;
    unhook_keys_o();
    destroy_window_o();
}
PORT_FN(0x00412b50, "Win32End", Win32End_rw, fp_win_window)
static void __cdecl destroy_window_rw() {
    kDestroyWindow(g_hwnd);
    kShowWindow(g_prev_fg, SW_SHOW);
    kSetForegroundWindow(g_prev_fg);
}
PORT_FN(0x00412b70, "destroy_window", destroy_window_rw, fp_win_window)
static void __cdecl unhook_keys_rw() {
    kUnhookWindowsHookEx(g_key_hook);
    g_key_hook = 0;
}
PORT_FN(0x00412ba0, "unhook_keys", unhook_keys_rw, fp_win_os)

// the getters
static HINSTANCE __cdecl Win32GetAppInstance_rw() { return g_instance; }
static char* __cdecl Win32GetArgs_rw() { return g_args; }
static HWND __cdecl Win32GetWindow_rw() { return g_hwnd; }
static const char* __cdecl Win32AppName_rw() { return k_app_name; }
static const char* __cdecl Win32GetUserDirectory_rw() { return g_user_dir; }
PORT_FN(0x00412bc0, "Win32GetAppInstance", Win32GetAppInstance_rw, fp_none)
PORT_FN(0x00412bd0, "Win32GetArgs", Win32GetArgs_rw, fp_none)
PORT_FN(0x00412be0, "Win32GetWindow", Win32GetWindow_rw, fp_none)
PORT_FN(0x00412cb0, "Win32AppName", Win32AppName_rw, fp_none)
PORT_FN(0x00412cc0, "Win32GetUserDirectory", Win32GetUserDirectory_rw, fp_none)

// Win32FlushRegistry (0x412cd0): every root key flushed
static void __cdecl Win32FlushRegistry_rw() {
    kRegFlushKey(HKEY_CLASSES_ROOT);
    kRegFlushKey(HKEY_CURRENT_CONFIG);
    kRegFlushKey(HKEY_CURRENT_USER);
    kRegFlushKey(HKEY_LOCAL_MACHINE);
    kRegFlushKey(HKEY_USERS);
    kRegFlushKey(HKEY_DYN_DATA);
}
PORT_FN(0x00412cd0, "Win32FlushRegistry", Win32FlushRegistry_rw, fp_win_os)

// Win32RegisterMessageHook (0x412d10) / Win32UnRegisterMessageHook (0x412d40): two slots (full: ignored)
static void __cdecl Win32RegisterMessageHook_rw(void* hook) {      // (a MsgHook_t; void* for test/fuzz.h)
    const MsgHook_t fn = (MsgHook_t)hook;
    for (int i = 0; i < 2; i++)
        if (g_msg_hooks[i] == 0) {
            g_msg_hooks[i] = fn;
            return;
        }
}
static void __cdecl Win32UnRegisterMessageHook_rw(void* hook) {    // (a MsgHook_t)
    const MsgHook_t fn = (MsgHook_t)hook;
    for (int i = 0; i < 2; i++)
        if (g_msg_hooks[i] == fn) {
            g_msg_hooks[i] = 0;
            return;
        }
}
static void fp_msg_hooks(Footprint& f, void*) { f.add((void*)0x00508060, 8, "message hooks"); }
PORT_FN(0x00412d10, "Win32RegisterMessageHook", Win32RegisterMessageHook_rw, fp_msg_hooks)
PORT_FN(0x00412d40, "Win32UnRegisterMessageHook", Win32UnRegisterMessageHook_rw, fp_msg_hooks)

// Win32GetTime (0x412d70) / Win32RestrictCursor (0x412d80)
static int __cdecl Win32GetTime_rw() { return (int)kTimeGetTime(); }
static void fp_time(Footprint& f) { f.replay_only = "it reads the clock"; }
PORT_FN(0x00412d70, "Win32GetTime", Win32GetTime_rw, fp_time)
static void __cdecl Win32RestrictCursor_rw() { restrict_cursor_o(1); }
PORT_FN(0x00412d80, "Win32RestrictCursor", Win32RestrictCursor_rw, fp_win_os)

// Win32System (0x412d90): the formatted command line (vsprintf into 0x1f8 bytes: unbounded) run hidden; waits,
// pumping messages every 250 ms, for its PRIMARY THREAD (not the process) to end; the process's exit code (still
// 259 if other threads run on). Neither handle is ever closed.
static uint8_t __cdecl Win32System_rw(int* err, int* code, const char* fmt, Va va) {
    if (err) *err = 0;
    char cmd[0x1f8];
    cmd[0] = 0;
    vsprintf_o(cmd, fmt, VA(va));
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = 0x44;
    si.dwFlags = STARTF_USESHOWWINDOW;                             // (wShowWindow SW_HIDE)
    PROCESS_INFORMATION pi;
    if (!kCreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
        if (err) *err = (int)kGetLastError();
        return 0;
    }
    while (kWaitForSingleObject(pi.hThread, 0xfa) == WAIT_TIMEOUT) Win32Idle();
    DWORD ec;
    if (!kGetExitCodeProcess(pi.hProcess, &ec)) {
        LogPanic(S(0x004e6074));                                   // "Can't GetExitCodeProcess?"
        return 0;
    }
    if (code) *code = (int)ec;
    return 1;
}
static void fp_win32_system(Footprint& f, int*, int*, const char*, Va) { f.replay_only = "it runs a program"; }
PORT_FN(0x00412d90, "Win32System", Win32System_rw, fp_win32_system)

// =======================================================================================================================
// msg.obj: messages over a mailslot, \\<machine>\mailslot\mgi\<name>, 0x104 bytes each (a sequence number and
// 0x100 characters, strncpy'd: unterminated at 0x100). The client and the server share the handle.
// =======================================================================================================================

// MsgClientBegin (0x415cf0): "\\" machine "\mailslot\mgi\" name, opened for writing (0x100 bytes: unbounded)
static uint8_t __cdecl MsgClientBegin_rw(const char* name, const char* machine) {
    char path[0x100];
    memcpy(path, S(0x004e6f0c), 3);                                // "\\\\"
    memset(path + 3, 0, 0xfd);
    str_cat(path, machine);
    str_cat(path, S(0x004e6f10));                                  // "\\mailslot\\mgi\\"
    str_cat(path, name);
    HANDLE h = kCreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    g_mailslot = h;
    return h != INVALID_HANDLE_VALUE ? 1 : 0;
}
static void fp_msg_begin(Footprint& f, const char*, const char*) { f.replay_only = "it opens a mailslot"; }
PORT_FN(0x00415cf0, "MsgClientBegin", MsgClientBegin_rw, fp_msg_begin)

// MsgClientSend (0x415df0): the next sequence number and the text, one write
static uint8_t __cdecl MsgClientSend_rw(const char* text) {
    ASSERT_MSG_msg(g_mailslot != INVALID_HANDLE_VALUE ? 1 : 0, S(0x004e6f20), 0x57);   // "msg.cpp: Module called after failed startup (line %d)"
    g_msg_seq = g_msg_seq + 1;
    strncpy_o(g_msg->text, text, 0x100);
    DWORD put;
    if (kWriteFile(g_mailslot, g_msg, 0x104, &put, 0) && put == 0x104) return 1;
    return 0;
}
static void fp_msg_send(Footprint& f, const char*) { f.replay_only = "it writes to a mailslot"; }
PORT_FN(0x00415df0, "MsgClientSend", MsgClientSend_rw, fp_msg_send)

// MsgClientEnd (0x415e80): closed (the handle isn't reset)
static void __cdecl MsgClientEnd_rw() {
    ASSERT_MSG_msg(g_mailslot != INVALID_HANDLE_VALUE ? 1 : 0, S(0x004e6f58), 0x6b);
    kCloseHandle(g_mailslot);
}
static void fp_msg_close(Footprint& f) { f.replay_only = "it closes a mailslot"; }
PORT_FN(0x00415e80, "MsgClientEnd", MsgClientEnd_rw, fp_msg_close)

// MsgServerBegin (0x415eb0): \\.\mailslot\mgi\<name>, 0x104-byte messages, 200 ms read timeout (nothing calls
// the server in v1.0)
static uint8_t __cdecl MsgServerBegin_rw(const char* name) {
    char path[0x100];
    memcpy(path, S(0x004e6f90), 0x12);                             // "\\\\.\\mailslot\\mgi\\"
    memset(path + 0x12, 0, 0xee);
    str_cat(path, name);
    HANDLE h = kCreateMailslotA(path, 0x104, 0xc8, 0);
    g_mailslot = h;
    return h != INVALID_HANDLE_VALUE ? 1 : 0;
}
static void fp_msg_server_begin(Footprint& f, const char*) { f.replay_only = "it makes a mailslot"; }
PORT_FN(0x00415eb0, "MsgServerBegin", MsgServerBegin_rw, fp_msg_server_begin)

// MsgServerRecv (0x415f60): one whole message (else 0): its number, its text strncpy'd into n bytes
static uint8_t __cdecl MsgServerRecv_rw(int* seq, char* out, int n) {
    ASSERT_MSG_msg(g_mailslot != INVALID_HANDLE_VALUE ? 1 : 0, S(0x004e6fa4), 0x8d);
    DWORD got = 0;
    if (g_mailslot == INVALID_HANDLE_VALUE) return 0;
    if (!kReadFile(g_mailslot, g_msg, 0x104, &got, 0) || got != 0x104) return 0;
    *seq = g_msg_seq;
    strncpy_o(out, g_msg->text, (size_t)n);
    return 1;
}
static void fp_msg_recv(Footprint& f, int*, char*, int) { f.replay_only = "it reads a mailslot"; }
PORT_FN(0x00415f60, "MsgServerRecv", MsgServerRecv_rw, fp_msg_recv)

// MsgServerEnd (0x416000)
static void __cdecl MsgServerEnd_rw() {
    ASSERT_MSG_msg(g_mailslot != INVALID_HANDLE_VALUE ? 1 : 0, S(0x004e6fdc), 0xa4);
    kCloseHandle(g_mailslot);
    g_mailslot = INVALID_HANDLE_VALUE;
}
PORT_FN(0x00416000, "MsgServerEnd", MsgServerEnd_rw, fp_msg_close)

// =======================================================================================================================
// shmem.obj: a block the writer task fills and the reader task reads without either waiting for the other: three
// buffers -- the one being written (wr), the latest complete one (ro) and a spare. release_wr_mem makes wr the new
// ro (the reader's buffer, if it holds the old ro, is skipped: the spare is written next); grab_ro_mem takes ro
// (waiting on the strobe until one has been written). The ASSERT_MSGs (a bare ret) are still called, their
// arguments made in the original's order: the owner's name, the block's name, TaskGetName(TaskGetID()), then
// TaskGetID() again for the condition.
// =======================================================================================================================
typedef ShmemBlock*(__fastcall* ShmCtor_t)(ShmemBlock*, Edx, const char*, int);
static const ShmCtor_t shmem_block_ctor_o = (ShmCtor_t)0x004d8460;
typedef void(__fastcall* ShmVoid_t)(ShmemBlock*, Edx);
static const ShmVoid_t shmem_block_dtor_o = (ShmVoid_t)0x004d8590;
static const ShmVoid_t release_wr_mem_o = (ShmVoid_t)0x004d86c0;
static const ShmVoid_t release_ro_mem_o = (ShmVoid_t)0x004d88d0;
typedef void*(__fastcall* ShmPtr_t)(ShmemBlock*, Edx);
static const ShmPtr_t grab_wr_mem_o = (ShmPtr_t)0x004d8660;
static const ShmPtr_t grab_ro_mem_o = (ShmPtr_t)0x004d87e0;
typedef uint8_t(__fastcall* ShmFlag_t)(ShmemBlock*, Edx);
static const ShmFlag_t peek_ro_mem_o = (ShmFlag_t)0x004d8760;

// "shmem: Thread "%s" tried WR/RO operation on "%s" owned by "%s"": asserted by each method but the constructor
static void shm_assert_owner(ShmemBlock* self, int32_t volatile* owner, const char* nobody, const char* fmt) {
    const char* who = *owner ? TaskGetName(*owner) : nobody;
    const char* name = *(const char* volatile*)&self->name;
    const char* me = TaskGetName(TaskGetID());
    const int cond = (TaskGetID() - *owner) == 0 ? 1 : 0;
    ASSERT_MSG_shmem(cond, fmt, me, name, who);
}

// shmem_block::shmem_block (0x4d8460): its lock and strobe (either missing panics), the three buffers (size each,
// one MemAlloc, zeroed, unchecked), all fresh; the first free table slot (or the next), under the lame mutex
static ShmemBlock* __fastcall shmem_block_ctor_rw(ShmemBlock* self, Edx, const char* name, int size) {
    volatile ShmemBlock* b = self;
    b->name = name;
    b->mutex = SyncMutexAlloc();
    b->strobe = SyncStrobeAlloc();
    b->reader = 0;
    b->writer = TaskGetID();
    b->reading = 0;
    if (b->mutex == 0 || b->strobe == 0) {
        LogPanic(S(0x00503a78), b->name);                          // "Can't alloc Mutex/Strobe for \"%s\""
        return self;
    }
    b->spare = &self->b;
    b->c.state = 1;
    b->b.state = 1;
    b->a.state = 1;
    b->wr = &self->a;
    b->ro = &self->c;
    uint8_t* mem = (uint8_t*)MemAlloc(size * 3);
    b->a.mem = mem;
    b->b.mem = mem + size;
    b->c.mem = mem + size * 2;
    memset(mem, 0, (uint32_t)size);
    memset(b->b.mem, 0, (uint32_t)size);
    memset(b->c.mem, 0, (uint32_t)size);
    SyncLameMutexLock(g_shm_mutex);
    b->index = 0;
    if (g_shm_count > 0)
        for (;;) {
            if (g_shm_blocks[b->index] == 0) break;
            b->index = b->index + 1;
            if (!(g_shm_count > b->index)) break;
        }
    if (b->index == g_shm_count) g_shm_count = g_shm_count + 1;
    SyncLameMutexUnlock(g_shm_mutex);
    return self;
}
static void fp_shm_ctor(Footprint& f, ShmemBlock*, Edx, const char*, int) { f.replay_only = "it allocates its memory, lock and strobe"; }
PORT_FN(0x004d8460, "shmem_block::shmem_block", shmem_block_ctor_rw, fp_shm_ctor)

// shmem_block::~shmem_block (0x4d8590)
static void __fastcall shmem_block_dtor_rw(ShmemBlock* self, Edx) {
    volatile ShmemBlock* b = self;
    shm_assert_owner(self, &b->writer, S(0x00503a9c), S(0x00503aa8));    // "<nobody>", "... tried WR operation ..."
    const char* who = b->reader ? TaskGetName(b->reader) : S(0x00503ae4);   // "<none>"
    ASSERT_MSG_shmem(b->reading == 0 ? 1 : 0, S(0x00503aec), b->name, who);  // "shmem \"%s\" was deleted while task \"%s\" was still reading."
    b->writer = 0;
    b->reader = 0;
    MemFree(b->a.mem);
    SyncMutexFree(&self->mutex);
    SyncStrobeFree(&self->strobe);
}
static void fp_shm_dtor(Footprint& f, ShmemBlock*, Edx) { f.replay_only = "it frees its memory, lock and strobe"; }
PORT_FN(0x004d8590, "shmem_block::~shmem_block", shmem_block_dtor_rw, fp_shm_dtor)

// shmem_block::grab_wr_mem (0x4d8660): the writer's buffer
static void* __fastcall grab_wr_mem_rw(ShmemBlock* self, Edx) {
    volatile ShmemBlock* b = self;
    shm_assert_owner(self, &b->writer, S(0x00503b28), S(0x00503b34));
    return b->wr->mem;
}
static void fp_shm_nothing(Footprint&, ShmemBlock*, Edx) {}
PORT_FN(0x004d8660, "shmem_block::grab_wr_mem", grab_wr_mem_rw, fp_shm_nothing)

// shmem_block::release_wr_mem (0x4d86c0): written -> the new ro; the old ro the next wr, unless the reader holds it
// (then the spare, and the old ro becomes the spare); the reader woken
static void __fastcall release_wr_mem_rw(ShmemBlock* self, Edx) {
    volatile ShmemBlock* b = self;
    shm_assert_owner(self, &b->writer, S(0x00503b70), S(0x00503b7c));
    SyncMutexLock(b->mutex);
    b->wr->state = 0;
    ShmemSlot* ro = b->ro;
    ShmemSlot* wr = b->wr;
    b->wr = ro;
    b->ro = wr;
    if (ro->state == 2) {
        b->wr = b->spare;
        b->spare = ro;
    }
    SyncMutexUnlock(b->mutex);
    SyncStrobeFlash(b->strobe);
}
static void fp_shm_release_wr(Footprint& f, ShmemBlock*, Edx) { f.replay_only = "it takes the block's lock and wakes the reader"; }
PORT_FN(0x004d86c0, "shmem_block::release_wr_mem", release_wr_mem_rw, fp_shm_release_wr)

// shmem_block::peek_ro_mem (0x4d8760): is there a written buffer the reader hasn't taken?
static uint8_t __fastcall peek_ro_mem_rw(ShmemBlock* self, Edx) {
    volatile ShmemBlock* b = self;
    shm_assert_owner(self, &b->reader, S(0x00503bb8), S(0x00503bc4));
    SyncMutexLock(b->mutex);
    ShmemSlot* ro = b->ro;
    void* m = b->mutex;
    const uint8_t fresh = *(volatile int32_t*)&ro->state == 0 ? 1 : 0;
    SyncMutexUnlock(m);
    return fresh;
}
static void fp_shm_peek(Footprint& f, ShmemBlock*, Edx) { f.replay_only = "it reads, under the block's lock, what the writer's thread changes"; }
PORT_FN(0x004d8760, "shmem_block::peek_ro_mem", peek_ro_mem_rw, fp_shm_peek)

// shmem_block::grab_ro_mem (0x4d87e0): ro taken (state 2) if it's been written; else the strobe waited on once and
// ro taken whatever it holds
static void* __fastcall grab_ro_mem_rw(ShmemBlock* self, Edx) {
    volatile ShmemBlock* b = self;
    shm_assert_owner(self, &b->reader, S(0x00503c00), S(0x00503c0c));
    {
        const char* name = b->name;
        const char* me = TaskGetName(TaskGetID());
        ASSERT_MSG_shmem(b->reading == 0 ? 1 : 0, S(0x00503c48), me, name);   // "Task \"%s\" re-grabbed shmem \"%s\""
    }
    void* mem = 0;
    SyncMutexLock(b->mutex);
    if (b->ro->state == 0) {
        b->ro->state = 2;
        ShmemSlot* ro = b->ro;
        mem = ro->mem;
        b->reading = ro;
    }
    SyncMutexUnlock(b->mutex);
    if (mem == 0) {
        SyncStrobeWait(b->strobe);
        SyncMutexLock(b->mutex);
        b->ro->state = 2;
        SyncMutexUnlock(b->mutex);
        ShmemSlot* ro = b->ro;
        mem = ro->mem;
        b->reading = ro;
    }
    return mem;
}
static void fp_shm_grab_ro(Footprint& f, ShmemBlock*, Edx) { f.replay_only = "it can wait for the writer"; }
PORT_FN(0x004d87e0, "shmem_block::grab_ro_mem", grab_ro_mem_rw, fp_shm_grab_ro)

// shmem_block::release_ro_mem (0x4d88d0): the held buffer fresh again (no lock; none held: a write to address 0)
static void __fastcall release_ro_mem_rw(ShmemBlock* self, Edx) {
    volatile ShmemBlock* b = self;
    shm_assert_owner(self, &b->reader, S(0x00503c68), S(0x00503c74));
    {
        const char* name = b->name;
        const char* me = TaskGetName(TaskGetID());
        ASSERT_MSG_shmem(b->reading != 0 ? 1 : 0, S(0x00503cb0), me, name);   // "Task \"%s\" read-released un-grabbed shmem \"%s\""
    }
    *(volatile int32_t*)&b->reading->state = 1;
    b->reading = 0;
}
static void fp_shm_release_ro(Footprint& f, ShmemBlock*, Edx) {
    // (it writes only the held slot's state and `reading`, but the writer's thread reads that state, under the
    // block's lock this doesn't take: a shadow check's restore would flip it back to "being read" under that thread)
    f.replay_only = "the writer's thread reads what it writes";
}
PORT_FN(0x004d88d0, "shmem_block::release_ro_mem", release_ro_mem_rw, fp_shm_release_ro)

// ShmemAlloc (0x4d8960): a block (0x40 MemAlloc'd) in the table; its handle is its index + 1
static uint32_t __cdecl ShmemAlloc_rw(const char* name, int size) {
    ShmemBlock* p = (ShmemBlock*)MemAlloc(0x40);
    ShmemBlock* b = p ? shmem_block_ctor_o(p, 0, name, size) : 0;
    if (b) {
        const uint32_t i = b->index;
        g_shm_blocks[i] = b;
        return i + 1;
    }
    LogPanic(S(0x00503cec), name);                                 // "Can't alloc shmem_block for \"%s\"!"
    return 0;
}
static void fp_shm_alloc(Footprint& f, const char*, int) { f.replay_only = "it allocates a shared block"; }
PORT_FN(0x004d8960, "ShmemAlloc", ShmemAlloc_rw, fp_shm_alloc)

// ShmemGet (0x4d89b0): by name (case-insensitive); the caller becomes its reader
static uint32_t __cdecl ShmemGet_rw(const char* name) {
    for (uint32_t i = 0; g_shm_count > i; i++) {
        ShmemBlock* b = g_shm_blocks[i];
        if (b && stricmp_o(name, b->name) == 0) {
            const int id = TaskGetID();
            ((volatile ShmemBlock*)g_shm_blocks[i])->reader = id;
            return i + 1;
        }
    }
    LogPanic(S(0x00503d10), name);                                 // "ShmemGet: \"%s\" has not been allocated"
    return 0;
}
static void fp_shm_get(Footprint& f, const char*) {
    for (uint32_t i = 0; i < *(volatile uint32_t*)0x00503a74 && i < 64; i++) {
        ShmemBlock* b = ((ShmemBlock**)0x005d5768)[i];
        if (b) f.add(&b->reader, 4, "shmem reader");
    }
}
PORT_FN(0x004d89b0, "ShmemGet", ShmemGet_rw, fp_shm_get)

// the handle functions: the table's entry handle - 1
#define SHM(h) (g_shm_blocks[(h) - 1])
static uint8_t __cdecl ShmemPeekReadable_rw(uint32_t h) { return peek_ro_mem_o(SHM(h), 0); }
static void fp_shm_h_peek(Footprint& f, uint32_t) { f.replay_only = "it reads, under the block's lock, what the writer's thread changes"; }
PORT_FN(0x004d8a20, "ShmemPeekReadable", ShmemPeekReadable_rw, fp_shm_h_peek)
static void* __cdecl ShmemGetReadable_rw(uint32_t h) {
    void* m = grab_ro_mem_o(SHM(h), 0);
    ASSERT_MSG_shmem(m != 0 ? 1 : 0, S(0x00503d38), SHM(h)->name);     // "Shmem \"%s\" would have returned NULL"
    return m;
}
static void fp_shm_h_grab_ro(Footprint& f, uint32_t) { f.replay_only = "it can wait for the writer"; }
PORT_FN(0x004d8a30, "ShmemGetReadable", ShmemGetReadable_rw, fp_shm_h_grab_ro)
static void* __cdecl ShmemGetWritable_rw(uint32_t h) { return grab_wr_mem_o(SHM(h), 0); }
static void fp_shm_h_nothing(Footprint&, uint32_t) {}
PORT_FN(0x004d8a70, "ShmemGetWritable", ShmemGetWritable_rw, fp_shm_h_nothing)
static void __cdecl ShmemReleaseReadable_rw(uint32_t h) { release_ro_mem_o(SHM(h), 0); }
static void fp_shm_h_release_ro(Footprint& f, uint32_t) { f.replay_only = "the writer's thread reads what it writes"; }
PORT_FN(0x004d8a80, "ShmemReleaseReadable", ShmemReleaseReadable_rw, fp_shm_h_release_ro)
static void __cdecl ShmemReleaseWritable_rw(uint32_t h) { release_wr_mem_o(SHM(h), 0); }
static void fp_shm_h_release_wr(Footprint& f, uint32_t) { f.replay_only = "it takes the block's lock and wakes the reader"; }
PORT_FN(0x004d8a90, "ShmemReleaseWritable", ShmemReleaseWritable_rw, fp_shm_h_release_wr)

// ShmemFree (0x4d8aa0): destroyed, deleted, its slot cleared (the count stays)
static void __cdecl ShmemFree_rw(uint32_t h) {
    ShmemBlock* b = SHM(h);
    if (b) {
        shmem_block_dtor_o(b, 0);
        operator_delete(b);
    }
    SHM(h) = 0;
}
static void fp_shm_free(Footprint& f, uint32_t) { f.replay_only = "it frees a shared block"; }
PORT_FN(0x004d8aa0, "ShmemFree", ShmemFree_rw, fp_shm_free)

// =======================================================================================================================
// winerror.obj
// =======================================================================================================================

// Win32GetErrorString(int) (0x416080): the error's name (its string in the game, as the original returns it), or
// "<unknown>" (0x4ea7a8). The original is a compiled switch; this is its table, extracted from it and checked
// against it for every code from -0x10000 to 0x30000 (and random ones) by test/world_krn_file.cpp.
namespace {
struct ErrName { int32_t code; uint32_t str; };
}
static const ErrName k_err_names[] = {
    {    0, 0x004e7014},   // NO_ERROR
    {    1, 0x004e7020},   // ERROR_INVALID_FUNCTION
    {    2, 0x004e7038},   // ERROR_FILE_NOT_FOUND
    {    3, 0x004e7050},   // ERROR_PATH_NOT_FOUND
    {    4, 0x004e7068},   // ERROR_TOO_MANY_OPEN_FILES
    {    5, 0x004e7084},   // ERROR_ACCESS_DENIED
    {    6, 0x004e7098},   // ERROR_INVALID_HANDLE
    {    7, 0x004e70b0},   // ERROR_ARENA_TRASHED
    {    8, 0x004e70c4},   // ERROR_NOT_ENOUGH_MEMORY
    {    9, 0x004e70dc},   // ERROR_INVALID_BLOCK
    {   10, 0x004e70f0},   // ERROR_BAD_ENVIRONMENT
    {   11, 0x004e7108},   // ERROR_BAD_FORMAT
    {   12, 0x004e711c},   // ERROR_INVALID_ACCESS
    {   13, 0x004e7134},   // ERROR_INVALID_DATA
    {   14, 0x004e7148},   // ERROR_OUTOFMEMORY
    {   15, 0x004e715c},   // ERROR_INVALID_DRIVE
    {   16, 0x004e7170},   // ERROR_CURRENT_DIRECTORY
    {   17, 0x004e7188},   // ERROR_NOT_SAME_DEVICE
    {   18, 0x004e71a0},   // ERROR_NO_MORE_FILES
    {   19, 0x004e71b4},   // ERROR_WRITE_PROTECT
    {   20, 0x004e71c8},   // ERROR_BAD_UNIT
    {   21, 0x004e71d8},   // ERROR_NOT_READY
    {   22, 0x004e71e8},   // ERROR_BAD_COMMAND
    {   23, 0x004e71fc},   // ERROR_CRC
    {   24, 0x004e7208},   // ERROR_BAD_LENGTH
    {   25, 0x004e721c},   // ERROR_SEEK
    {   26, 0x004e7228},   // ERROR_NOT_DOS_DISK
    {   27, 0x004e723c},   // ERROR_SECTOR_NOT_FOUND
    {   28, 0x004e7254},   // ERROR_OUT_OF_PAPER
    {   29, 0x004e7268},   // ERROR_WRITE_FAULT
    {   30, 0x004e727c},   // ERROR_READ_FAULT
    {   31, 0x004e7290},   // ERROR_GEN_FAILURE
    {   32, 0x004e72a4},   // ERROR_SHARING_VIOLATION
    {   33, 0x004e72bc},   // ERROR_LOCK_VIOLATION
    {   34, 0x004e72d4},   // ERROR_WRONG_DISK
    {   36, 0x004e72e8},   // ERROR_SHARING_BUFFER_EXCEEDED
    {   38, 0x004e7308},   // ERROR_HANDLE_EOF
    {   39, 0x004e731c},   // ERROR_HANDLE_DISK_FULL
    {   50, 0x004e7334},   // ERROR_NOT_SUPPORTED
    {   51, 0x004e7348},   // ERROR_REM_NOT_LIST
    {   52, 0x004e735c},   // ERROR_DUP_NAME
    {   53, 0x004e736c},   // ERROR_BAD_NETPATH
    {   54, 0x004e7380},   // ERROR_NETWORK_BUSY
    {   55, 0x004e7394},   // ERROR_DEV_NOT_EXIST
    {   56, 0x004e73a8},   // ERROR_TOO_MANY_CMDS
    {   57, 0x004e73bc},   // ERROR_ADAP_HDW_ERR
    {   58, 0x004e73d0},   // ERROR_BAD_NET_RESP
    {   59, 0x004e73e4},   // ERROR_UNEXP_NET_ERR
    {   60, 0x004e73f8},   // ERROR_BAD_REM_ADAP
    {   61, 0x004e740c},   // ERROR_PRINTQ_FULL
    {   62, 0x004e7420},   // ERROR_NO_SPOOL_SPACE
    {   63, 0x004e7438},   // ERROR_PRINT_CANCELLED
    {   64, 0x004e7450},   // ERROR_NETNAME_DELETED
    {   65, 0x004e7468},   // ERROR_NETWORK_ACCESS_DENIED
    {   66, 0x004e7484},   // ERROR_BAD_DEV_TYPE
    {   67, 0x004e7498},   // ERROR_BAD_NET_NAME
    {   68, 0x004e74ac},   // ERROR_TOO_MANY_NAMES
    {   69, 0x004e74c4},   // ERROR_TOO_MANY_SESS
    {   70, 0x004e74d8},   // ERROR_SHARING_PAUSED
    {   71, 0x004e74f0},   // ERROR_REQ_NOT_ACCEP
    {   72, 0x004e7504},   // ERROR_REDIR_PAUSED
    {   80, 0x004e7518},   // ERROR_FILE_EXISTS
    {   82, 0x004e752c},   // ERROR_CANNOT_MAKE
    {   83, 0x004e7540},   // ERROR_FAIL_I24
    {   84, 0x004e7550},   // ERROR_OUT_OF_STRUCTURES
    {   85, 0x004e7568},   // ERROR_ALREADY_ASSIGNED
    {   86, 0x004e7580},   // ERROR_INVALID_PASSWORD
    {   87, 0x004e7598},   // ERROR_INVALID_PARAMETER
    {   88, 0x004e75b0},   // ERROR_NET_WRITE_FAULT
    {   89, 0x004e75c8},   // ERROR_NO_PROC_SLOTS
    {  100, 0x004e75dc},   // ERROR_TOO_MANY_SEMAPHORES
    {  101, 0x004e75f8},   // ERROR_EXCL_SEM_ALREADY_OWNED
    {  102, 0x004e7618},   // ERROR_SEM_IS_SET
    {  103, 0x004e762c},   // ERROR_TOO_MANY_SEM_REQUESTS
    {  104, 0x004e7648},   // ERROR_INVALID_AT_INTERRUPT_TIME
    {  105, 0x004e7668},   // ERROR_SEM_OWNER_DIED
    {  106, 0x004e7680},   // ERROR_SEM_USER_LIMIT
    {  107, 0x004e7698},   // ERROR_DISK_CHANGE
    {  108, 0x004e76ac},   // ERROR_DRIVE_LOCKED
    {  109, 0x004e76c0},   // ERROR_BROKEN_PIPE
    {  110, 0x004e76d4},   // ERROR_OPEN_FAILED
    {  111, 0x004e76e8},   // ERROR_BUFFER_OVERFLOW
    {  112, 0x004e7700},   // ERROR_DISK_FULL
    {  113, 0x004e7710},   // ERROR_NO_MORE_SEARCH_HANDLES
    {  114, 0x004e7730},   // ERROR_INVALID_TARGET_HANDLE
    {  117, 0x004e774c},   // ERROR_INVALID_CATEGORY
    {  118, 0x004e7764},   // ERROR_INVALID_VERIFY_SWITCH
    {  119, 0x004e7780},   // ERROR_BAD_DRIVER_LEVEL
    {  120, 0x004e7798},   // ERROR_CALL_NOT_IMPLEMENTED
    {  121, 0x004e77b4},   // ERROR_SEM_TIMEOUT
    {  122, 0x004e77c8},   // ERROR_INSUFFICIENT_BUFFER
    {  123, 0x004e77e4},   // ERROR_INVALID_NAME
    {  124, 0x004e77f8},   // ERROR_INVALID_LEVEL
    {  125, 0x004e780c},   // ERROR_NO_VOLUME_LABEL
    {  126, 0x004e7824},   // ERROR_MOD_NOT_FOUND
    {  127, 0x004e7838},   // ERROR_PROC_NOT_FOUND
    {  128, 0x004e7850},   // ERROR_WAIT_NO_CHILDREN
    {  129, 0x004e7868},   // ERROR_CHILD_NOT_COMPLETE
    {  130, 0x004e7884},   // ERROR_DIRECT_ACCESS_HANDLE
    {  131, 0x004e78a0},   // ERROR_NEGATIVE_SEEK
    {  132, 0x004e78b4},   // ERROR_SEEK_ON_DEVICE
    {  133, 0x004e78cc},   // ERROR_IS_JOIN_TARGET
    {  134, 0x004e78e4},   // ERROR_IS_JOINED
    {  135, 0x004e78f4},   // ERROR_IS_SUBSTED
    {  136, 0x004e7908},   // ERROR_NOT_JOINED
    {  137, 0x004e791c},   // ERROR_NOT_SUBSTED
    {  138, 0x004e7930},   // ERROR_JOIN_TO_JOIN
    {  139, 0x004e7944},   // ERROR_SUBST_TO_SUBST
    {  140, 0x004e795c},   // ERROR_JOIN_TO_SUBST
    {  141, 0x004e7970},   // ERROR_SUBST_TO_JOIN
    {  142, 0x004e7984},   // ERROR_BUSY_DRIVE
    {  143, 0x004e7998},   // ERROR_SAME_DRIVE
    {  144, 0x004e79ac},   // ERROR_DIR_NOT_ROOT
    {  145, 0x004e79c0},   // ERROR_DIR_NOT_EMPTY
    {  146, 0x004e79d4},   // ERROR_IS_SUBST_PATH
    {  147, 0x004e79e8},   // ERROR_IS_JOIN_PATH
    {  148, 0x004e79fc},   // ERROR_PATH_BUSY
    {  149, 0x004e7a0c},   // ERROR_IS_SUBST_TARGET
    {  150, 0x004e7a24},   // ERROR_SYSTEM_TRACE
    {  151, 0x004e7a38},   // ERROR_INVALID_EVENT_COUNT
    {  152, 0x004e7a54},   // ERROR_TOO_MANY_MUXWAITERS
    {  153, 0x004e7a70},   // ERROR_INVALID_LIST_FORMAT
    {  154, 0x004e7a8c},   // ERROR_LABEL_TOO_LONG
    {  155, 0x004e7aa4},   // ERROR_TOO_MANY_TCBS
    {  156, 0x004e7ab8},   // ERROR_SIGNAL_REFUSED
    {  157, 0x004e7ad0},   // ERROR_DISCARDED
    {  158, 0x004e7ae0},   // ERROR_NOT_LOCKED
    {  159, 0x004e7af4},   // ERROR_BAD_THREADID_ADDR
    {  160, 0x004e7b0c},   // ERROR_BAD_ARGUMENTS
    {  161, 0x004e7b20},   // ERROR_BAD_PATHNAME
    {  162, 0x004e7b34},   // ERROR_SIGNAL_PENDING
    {  164, 0x004e7b4c},   // ERROR_MAX_THRDS_REACHED
    {  167, 0x004e7b64},   // ERROR_LOCK_FAILED
    {  170, 0x004e7b78},   // ERROR_BUSY
    {  173, 0x004e7b84},   // ERROR_CANCEL_VIOLATION
    {  174, 0x004e7b9c},   // ERROR_ATOMIC_LOCKS_NOT_SUPPORTED
    {  180, 0x004e7bc0},   // ERROR_INVALID_SEGMENT_NUMBER
    {  182, 0x004e7be0},   // ERROR_INVALID_ORDINAL
    {  183, 0x004e7bf8},   // ERROR_ALREADY_EXISTS
    {  186, 0x004e7c10},   // ERROR_INVALID_FLAG_NUMBER
    {  187, 0x004e7c2c},   // ERROR_SEM_NOT_FOUND
    {  188, 0x004e7c40},   // ERROR_INVALID_STARTING_CODESEG
    {  189, 0x004e7c60},   // ERROR_INVALID_STACKSEG
    {  190, 0x004e7c78},   // ERROR_INVALID_MODULETYPE
    {  191, 0x004e7c94},   // ERROR_INVALID_EXE_SIGNATURE
    {  192, 0x004e7cb0},   // ERROR_EXE_MARKED_INVALID
    {  193, 0x004e7ccc},   // ERROR_BAD_EXE_FORMAT
    {  194, 0x004e7ce4},   // ERROR_ITERATED_DATA_EXCEEDS_64k
    {  195, 0x004e7d04},   // ERROR_INVALID_MINALLOCSIZE
    {  196, 0x004e7d20},   // ERROR_DYNLINK_FROM_INVALID_RING
    {  197, 0x004e7d40},   // ERROR_IOPL_NOT_ENABLED
    {  198, 0x004e7d58},   // ERROR_INVALID_SEGDPL
    {  199, 0x004e7d70},   // ERROR_AUTODATASEG_EXCEEDS_64k
    {  200, 0x004e7d90},   // ERROR_RING2SEG_MUST_BE_MOVABLE
    {  201, 0x004e7db0},   // ERROR_RELOC_CHAIN_XEEDS_SEGLIM
    {  202, 0x004e7dd0},   // ERROR_INFLOOP_IN_RELOC_CHAIN
    {  203, 0x004e7df0},   // ERROR_ENVVAR_NOT_FOUND
    {  205, 0x004e7e08},   // ERROR_NO_SIGNAL_SENT
    {  206, 0x004e7e20},   // ERROR_FILENAME_EXCED_RANGE
    {  207, 0x004e7e3c},   // ERROR_RING2_STACK_IN_USE
    {  208, 0x004e7e58},   // ERROR_META_EXPANSION_TOO_LONG
    {  209, 0x004e7e78},   // ERROR_INVALID_SIGNAL_NUMBER
    {  210, 0x004e7e94},   // ERROR_THREAD_1_INACTIVE
    {  212, 0x004e7eac},   // ERROR_LOCKED
    {  214, 0x004e7ebc},   // ERROR_TOO_MANY_MODULES
    {  215, 0x004e7ed4},   // ERROR_NESTING_NOT_ALLOWED
    {  230, 0x004e7ef0},   // ERROR_BAD_PIPE
    {  231, 0x004e7f00},   // ERROR_PIPE_BUSY
    {  232, 0x004e7f10},   // ERROR_NO_DATA
    {  233, 0x004e7f20},   // ERROR_PIPE_NOT_CONNECTED
    {  234, 0x004e7f3c},   // ERROR_MORE_DATA
    {  240, 0x004e7f4c},   // ERROR_VC_DISCONNECTED
    {  254, 0x004e7f64},   // ERROR_INVALID_EA_NAME
    {  255, 0x004e7f7c},   // ERROR_EA_LIST_INCONSISTENT
    {  259, 0x004e7f98},   // ERROR_NO_MORE_ITEMS
    {  266, 0x004e7fac},   // ERROR_CANNOT_COPY
    {  267, 0x004e7fc0},   // ERROR_DIRECTORY
    {  275, 0x004e7fd0},   // ERROR_EAS_DIDNT_FIT
    {  276, 0x004e7fe4},   // ERROR_EA_FILE_CORRUPT
    {  277, 0x004e7ffc},   // ERROR_EA_TABLE_FULL
    {  278, 0x004e8010},   // ERROR_INVALID_EA_HANDLE
    {  282, 0x004e8028},   // ERROR_EAS_NOT_SUPPORTED
    {  288, 0x004e8040},   // ERROR_NOT_OWNER
    {  298, 0x004e8050},   // ERROR_TOO_MANY_POSTS
    {  299, 0x004e8068},   // ERROR_PARTIAL_COPY
    {  317, 0x004e807c},   // ERROR_MR_MID_NOT_FOUND
    {  487, 0x004e8094},   // ERROR_INVALID_ADDRESS
    {  534, 0x004e80ac},   // ERROR_ARITHMETIC_OVERFLOW
    {  535, 0x004e80c8},   // ERROR_PIPE_CONNECTED
    {  536, 0x004e80e0},   // ERROR_PIPE_LISTENING
    {  994, 0x004e80f8},   // ERROR_EA_ACCESS_DENIED
    {  995, 0x004e8110},   // ERROR_OPERATION_ABORTED
    {  996, 0x004e8128},   // ERROR_IO_INCOMPLETE
    {  997, 0x004e813c},   // ERROR_IO_PENDING
    {  998, 0x004e8150},   // ERROR_NOACCESS
    {  999, 0x004e8160},   // ERROR_SWAPERROR
    { 1001, 0x004e8170},   // ERROR_STACK_OVERFLOW
    { 1002, 0x004e8188},   // ERROR_INVALID_MESSAGE
    { 1003, 0x004e81a0},   // ERROR_CAN_NOT_COMPLETE
    { 1004, 0x004e81b8},   // ERROR_INVALID_FLAGS
    { 1005, 0x004e81cc},   // ERROR_UNRECOGNIZED_VOLUME
    { 1006, 0x004e81e8},   // ERROR_FILE_INVALID
    { 1007, 0x004e81fc},   // ERROR_FULLSCREEN_MODE
    { 1008, 0x004e8214},   // ERROR_NO_TOKEN
    { 1009, 0x004e8224},   // ERROR_BADDB
    { 1010, 0x004e8230},   // ERROR_BADKEY
    { 1011, 0x004e8240},   // ERROR_CANTOPEN
    { 1012, 0x004e8250},   // ERROR_CANTREAD
    { 1013, 0x004e8260},   // ERROR_CANTWRITE
    { 1014, 0x004e8270},   // ERROR_REGISTRY_RECOVERED
    { 1015, 0x004e828c},   // ERROR_REGISTRY_CORRUPT
    { 1016, 0x004e82a4},   // ERROR_REGISTRY_IO_FAILED
    { 1017, 0x004e82c0},   // ERROR_NOT_REGISTRY_FILE
    { 1018, 0x004e82d8},   // ERROR_KEY_DELETED
    { 1019, 0x004e82ec},   // ERROR_NO_LOG_SPACE
    { 1020, 0x004e8300},   // ERROR_KEY_HAS_CHILDREN
    { 1021, 0x004e8318},   // ERROR_CHILD_MUST_BE_VOLATILE
    { 1022, 0x004e8338},   // ERROR_NOTIFY_ENUM_DIR
    { 1051, 0x004e8350},   // ERROR_DEPENDENT_SERVICES_RUNNING
    { 1052, 0x004e8374},   // ERROR_INVALID_SERVICE_CONTROL
    { 1053, 0x004e8394},   // ERROR_SERVICE_REQUEST_TIMEOUT
    { 1054, 0x004e83b4},   // ERROR_SERVICE_NO_THREAD
    { 1055, 0x004e83cc},   // ERROR_SERVICE_DATABASE_LOCKED
    { 1056, 0x004e83ec},   // ERROR_SERVICE_ALREADY_RUNNING
    { 1057, 0x004e840c},   // ERROR_INVALID_SERVICE_ACCOUNT
    { 1058, 0x004e842c},   // ERROR_SERVICE_DISABLED
    { 1059, 0x004e8444},   // ERROR_CIRCULAR_DEPENDENCY
    { 1060, 0x004e8460},   // ERROR_SERVICE_DOES_NOT_EXIST
    { 1061, 0x004e8480},   // ERROR_SERVICE_CANNOT_ACCEPT_CTRL
    { 1062, 0x004e84a4},   // ERROR_SERVICE_NOT_ACTIVE
    { 1063, 0x004e84c0},   // ERROR_FAILED_SERVICE_CONTROLLER_CONNECT
    { 1064, 0x004e84e8},   // ERROR_EXCEPTION_IN_SERVICE
    { 1065, 0x004e8504},   // ERROR_DATABASE_DOES_NOT_EXIST
    { 1066, 0x004e8524},   // ERROR_SERVICE_SPECIFIC_ERROR
    { 1067, 0x004e8544},   // ERROR_PROCESS_ABORTED
    { 1068, 0x004e855c},   // ERROR_SERVICE_DEPENDENCY_FAIL
    { 1069, 0x004e857c},   // ERROR_SERVICE_LOGON_FAILED
    { 1070, 0x004e8598},   // ERROR_SERVICE_START_HANG
    { 1071, 0x004e85b4},   // ERROR_INVALID_SERVICE_LOCK
    { 1072, 0x004e85d0},   // ERROR_SERVICE_MARKED_FOR_DELETE
    { 1073, 0x004e85f0},   // ERROR_SERVICE_EXISTS
    { 1074, 0x004e8608},   // ERROR_ALREADY_RUNNING_LKG
    { 1075, 0x004e8624},   // ERROR_SERVICE_DEPENDENCY_DELETED
    { 1076, 0x004e8648},   // ERROR_BOOT_ALREADY_ACCEPTED
    { 1077, 0x004e8664},   // ERROR_SERVICE_NEVER_STARTED
    { 1078, 0x004e8680},   // ERROR_DUPLICATE_SERVICE_NAME
    { 1100, 0x004e86a0},   // ERROR_END_OF_MEDIA
    { 1101, 0x004e86b4},   // ERROR_FILEMARK_DETECTED
    { 1102, 0x004e86cc},   // ERROR_BEGINNING_OF_MEDIA
    { 1103, 0x004e86e8},   // ERROR_SETMARK_DETECTED
    { 1104, 0x004e8700},   // ERROR_NO_DATA_DETECTED
    { 1105, 0x004e8718},   // ERROR_PARTITION_FAILURE
    { 1106, 0x004e8730},   // ERROR_INVALID_BLOCK_LENGTH
    { 1107, 0x004e874c},   // ERROR_DEVICE_NOT_PARTITIONED
    { 1108, 0x004e876c},   // ERROR_UNABLE_TO_LOCK_MEDIA
    { 1109, 0x004e8788},   // ERROR_UNABLE_TO_UNLOAD_MEDIA
    { 1110, 0x004e87a8},   // ERROR_MEDIA_CHANGED
    { 1111, 0x004e87bc},   // ERROR_BUS_RESET
    { 1112, 0x004e87cc},   // ERROR_NO_MEDIA_IN_DRIVE
    { 1113, 0x004e87e4},   // ERROR_NO_UNICODE_TRANSLATION
    { 1114, 0x004e8804},   // ERROR_DLL_INIT_FAILED
    { 1115, 0x004e881c},   // ERROR_SHUTDOWN_IN_PROGRESS
    { 1116, 0x004e8838},   // ERROR_NO_SHUTDOWN_IN_PROGRESS
    { 1117, 0x004e8858},   // ERROR_IO_DEVICE
    { 1118, 0x004e8868},   // ERROR_SERIAL_NO_DEVICE
    { 1119, 0x004e8880},   // ERROR_IRQ_BUSY
    { 1120, 0x004e8890},   // ERROR_MORE_WRITES
    { 1121, 0x004e88a4},   // ERROR_COUNTER_TIMEOUT
    { 1122, 0x004e88bc},   // ERROR_FLOPPY_ID_MARK_NOT_FOUND
    { 1123, 0x004e88dc},   // ERROR_FLOPPY_WRONG_CYLINDER
    { 1124, 0x004e88f8},   // ERROR_FLOPPY_UNKNOWN_ERROR
    { 1125, 0x004e8914},   // ERROR_FLOPPY_BAD_REGISTERS
    { 1126, 0x004e8930},   // ERROR_DISK_RECALIBRATE_FAILED
    { 1127, 0x004e8950},   // ERROR_DISK_OPERATION_FAILED
    { 1128, 0x004e896c},   // ERROR_DISK_RESET_FAILED
    { 1129, 0x004e8984},   // ERROR_EOM_OVERFLOW
    { 1130, 0x004e8998},   // ERROR_NOT_ENOUGH_SERVER_MEMORY
    { 1131, 0x004e89b8},   // ERROR_POSSIBLE_DEADLOCK
    { 1132, 0x004e89d0},   // ERROR_MAPPED_ALIGNMENT
    { 1140, 0x004e89e8},   // ERROR_SET_POWER_STATE_VETOED
    { 1141, 0x004e8a08},   // ERROR_SET_POWER_STATE_FAILED
    { 1150, 0x004e8a28},   // ERROR_OLD_WIN_VERSION
    { 1151, 0x004e8a40},   // ERROR_APP_WRONG_OS
    { 1152, 0x004e8a54},   // ERROR_SINGLE_INSTANCE_APP
    { 1153, 0x004e8a70},   // ERROR_RMODE_APP
    { 1154, 0x004e8a80},   // ERROR_INVALID_DLL
    { 1155, 0x004e8a94},   // ERROR_NO_ASSOCIATION
    { 1156, 0x004e8aac},   // ERROR_DDE_FAIL
    { 1157, 0x004e8abc},   // ERROR_DLL_NOT_FOUND
    { 1200, 0x004e8b3c},   // ERROR_BAD_DEVICE
    { 1201, 0x004e8b50},   // ERROR_CONNECTION_UNAVAIL
    { 1202, 0x004e8b6c},   // ERROR_DEVICE_ALREADY_REMEMBERED
    { 1203, 0x004e8b8c},   // ERROR_NO_NET_OR_BAD_PATH
    { 1204, 0x004e8ba8},   // ERROR_BAD_PROVIDER
    { 1205, 0x004e8bbc},   // ERROR_CANNOT_OPEN_PROFILE
    { 1206, 0x004e8bd8},   // ERROR_BAD_PROFILE
    { 1207, 0x004e8bec},   // ERROR_NOT_CONTAINER
    { 1208, 0x004e8c00},   // ERROR_EXTENDED_ERROR
    { 1209, 0x004e8c18},   // ERROR_INVALID_GROUPNAME
    { 1210, 0x004e8c30},   // ERROR_INVALID_COMPUTERNAME
    { 1211, 0x004e8c4c},   // ERROR_INVALID_EVENTNAME
    { 1212, 0x004e8c64},   // ERROR_INVALID_DOMAINNAME
    { 1213, 0x004e8c80},   // ERROR_INVALID_SERVICENAME
    { 1214, 0x004e8c9c},   // ERROR_INVALID_NETNAME
    { 1215, 0x004e8cb4},   // ERROR_INVALID_SHARENAME
    { 1216, 0x004e8ccc},   // ERROR_INVALID_PASSWORDNAME
    { 1217, 0x004e8ce8},   // ERROR_INVALID_MESSAGENAME
    { 1218, 0x004e8d04},   // ERROR_INVALID_MESSAGEDEST
    { 1219, 0x004e8d20},   // ERROR_SESSION_CREDENTIAL_CONFLICT
    { 1220, 0x004e8d44},   // ERROR_REMOTE_SESSION_LIMIT_EXCEEDED
    { 1221, 0x004e8d68},   // ERROR_DUP_DOMAINNAME
    { 1222, 0x004e8d80},   // ERROR_NO_NETWORK
    { 1223, 0x004e8d94},   // ERROR_CANCELLED
    { 1224, 0x004e8da4},   // ERROR_USER_MAPPED_FILE
    { 1225, 0x004e8dbc},   // ERROR_CONNECTION_REFUSED
    { 1226, 0x004e8dd8},   // ERROR_GRACEFUL_DISCONNECT
    { 1227, 0x004e8df4},   // ERROR_ADDRESS_ALREADY_ASSOCIATED
    { 1228, 0x004e8e18},   // ERROR_ADDRESS_NOT_ASSOCIATED
    { 1229, 0x004e8e38},   // ERROR_CONNECTION_INVALID
    { 1230, 0x004e8e54},   // ERROR_CONNECTION_ACTIVE
    { 1231, 0x004e8e6c},   // ERROR_NETWORK_UNREACHABLE
    { 1232, 0x004e8e88},   // ERROR_HOST_UNREACHABLE
    { 1233, 0x004e8ea0},   // ERROR_PROTOCOL_UNREACHABLE
    { 1234, 0x004e8ebc},   // ERROR_PORT_UNREACHABLE
    { 1235, 0x004e8ed4},   // ERROR_REQUEST_ABORTED
    { 1236, 0x004e8eec},   // ERROR_CONNECTION_ABORTED
    { 1237, 0x004e8f08},   // ERROR_RETRY
    { 1238, 0x004e8f14},   // ERROR_CONNECTION_COUNT_LIMIT
    { 1239, 0x004e8f34},   // ERROR_LOGIN_TIME_RESTRICTION
    { 1240, 0x004e8f54},   // ERROR_LOGIN_WKSTA_RESTRICTION
    { 1241, 0x004e8f74},   // ERROR_INCORRECT_ADDRESS
    { 1242, 0x004e8f8c},   // ERROR_ALREADY_REGISTERED
    { 1243, 0x004e8fa8},   // ERROR_SERVICE_NOT_FOUND
    { 1244, 0x004e8fc0},   // ERROR_NOT_AUTHENTICATED
    { 1245, 0x004e8fd8},   // ERROR_NOT_LOGGED_ON
    { 1246, 0x004e8fec},   // ERROR_CONTINUE
    { 1247, 0x004e8ffc},   // ERROR_ALREADY_INITIALIZED
    { 1248, 0x004e9018},   // ERROR_NO_MORE_DEVICES
    { 1300, 0x004e9030},   // ERROR_NOT_ALL_ASSIGNED
    { 1301, 0x004e9048},   // ERROR_SOME_NOT_MAPPED
    { 1302, 0x004e9060},   // ERROR_NO_QUOTAS_FOR_ACCOUNT
    { 1303, 0x004e907c},   // ERROR_LOCAL_USER_SESSION_KEY
    { 1304, 0x004e909c},   // ERROR_NULL_LM_PASSWORD
    { 1305, 0x004e90b4},   // ERROR_UNKNOWN_REVISION
    { 1306, 0x004e90cc},   // ERROR_REVISION_MISMATCH
    { 1307, 0x004e90e4},   // ERROR_INVALID_OWNER
    { 1308, 0x004e90f8},   // ERROR_INVALID_PRIMARY_GROUP
    { 1309, 0x004e9114},   // ERROR_NO_IMPERSONATION_TOKEN
    { 1310, 0x004e9134},   // ERROR_CANT_DISABLE_MANDATORY
    { 1311, 0x004e9154},   // ERROR_NO_LOGON_SERVERS
    { 1312, 0x004e916c},   // ERROR_NO_SUCH_LOGON_SESSION
    { 1313, 0x004e9188},   // ERROR_NO_SUCH_PRIVILEGE
    { 1314, 0x004e91a0},   // ERROR_PRIVILEGE_NOT_HELD
    { 1315, 0x004e91bc},   // ERROR_INVALID_ACCOUNT_NAME
    { 1316, 0x004e91d8},   // ERROR_USER_EXISTS
    { 1317, 0x004e91ec},   // ERROR_NO_SUCH_USER
    { 1318, 0x004e9200},   // ERROR_GROUP_EXISTS
    { 1319, 0x004e9214},   // ERROR_NO_SUCH_GROUP
    { 1320, 0x004e9228},   // ERROR_MEMBER_IN_GROUP
    { 1321, 0x004e9240},   // ERROR_MEMBER_NOT_IN_GROUP
    { 1322, 0x004e925c},   // ERROR_LAST_ADMIN
    { 1323, 0x004e9270},   // ERROR_WRONG_PASSWORD
    { 1324, 0x004e9288},   // ERROR_ILL_FORMED_PASSWORD
    { 1325, 0x004e92a4},   // ERROR_PASSWORD_RESTRICTION
    { 1326, 0x004e92c0},   // ERROR_LOGON_FAILURE
    { 1327, 0x004e92d4},   // ERROR_ACCOUNT_RESTRICTION
    { 1328, 0x004e92f0},   // ERROR_INVALID_LOGON_HOURS
    { 1329, 0x004e930c},   // ERROR_INVALID_WORKSTATION
    { 1330, 0x004e9328},   // ERROR_PASSWORD_EXPIRED
    { 1331, 0x004e9340},   // ERROR_ACCOUNT_DISABLED
    { 1332, 0x004e9358},   // ERROR_NONE_MAPPED
    { 1333, 0x004e936c},   // ERROR_TOO_MANY_LUIDS_REQUESTED
    { 1334, 0x004e938c},   // ERROR_LUIDS_EXHAUSTED
    { 1335, 0x004e93a4},   // ERROR_INVALID_SUB_AUTHORITY
    { 1336, 0x004e93c0},   // ERROR_INVALID_ACL
    { 1337, 0x004e93d4},   // ERROR_INVALID_SID
    { 1338, 0x004e93e8},   // ERROR_INVALID_SECURITY_DESCR
    { 1340, 0x004e9408},   // ERROR_BAD_INHERITANCE_ACL
    { 1341, 0x004e9424},   // ERROR_SERVER_DISABLED
    { 1342, 0x004e943c},   // ERROR_SERVER_NOT_DISABLED
    { 1343, 0x004e9458},   // ERROR_INVALID_ID_AUTHORITY
    { 1344, 0x004e9474},   // ERROR_ALLOTTED_SPACE_EXCEEDED
    { 1345, 0x004e9494},   // ERROR_INVALID_GROUP_ATTRIBUTES
    { 1346, 0x004e94b4},   // ERROR_BAD_IMPERSONATION_LEVEL
    { 1347, 0x004e94d4},   // ERROR_CANT_OPEN_ANONYMOUS
    { 1348, 0x004e94f0},   // ERROR_BAD_VALIDATION_CLASS
    { 1349, 0x004e950c},   // ERROR_BAD_TOKEN_TYPE
    { 1350, 0x004e9524},   // ERROR_NO_SECURITY_ON_OBJECT
    { 1351, 0x004e9540},   // ERROR_CANT_ACCESS_DOMAIN_INFO
    { 1352, 0x004e9560},   // ERROR_INVALID_SERVER_STATE
    { 1353, 0x004e957c},   // ERROR_INVALID_DOMAIN_STATE
    { 1354, 0x004e9598},   // ERROR_INVALID_DOMAIN_ROLE
    { 1355, 0x004e95b4},   // ERROR_NO_SUCH_DOMAIN
    { 1356, 0x004e95cc},   // ERROR_DOMAIN_EXISTS
    { 1357, 0x004e95e0},   // ERROR_DOMAIN_LIMIT_EXCEEDED
    { 1358, 0x004e95fc},   // ERROR_INTERNAL_DB_CORRUPTION
    { 1359, 0x004e961c},   // ERROR_INTERNAL_ERROR
    { 1360, 0x004e9634},   // ERROR_GENERIC_NOT_MAPPED
    { 1361, 0x004e9650},   // ERROR_BAD_DESCRIPTOR_FORMAT
    { 1362, 0x004e966c},   // ERROR_NOT_LOGON_PROCESS
    { 1363, 0x004e9684},   // ERROR_LOGON_SESSION_EXISTS
    { 1364, 0x004e96a0},   // ERROR_NO_SUCH_PACKAGE
    { 1365, 0x004e96b8},   // ERROR_BAD_LOGON_SESSION_STATE
    { 1366, 0x004e96d8},   // ERROR_LOGON_SESSION_COLLISION
    { 1367, 0x004e96f8},   // ERROR_INVALID_LOGON_TYPE
    { 1368, 0x004e9714},   // ERROR_CANNOT_IMPERSONATE
    { 1369, 0x004e9730},   // ERROR_RXACT_INVALID_STATE
    { 1370, 0x004e974c},   // ERROR_RXACT_COMMIT_FAILURE
    { 1371, 0x004e9768},   // ERROR_SPECIAL_ACCOUNT
    { 1372, 0x004e9780},   // ERROR_SPECIAL_GROUP
    { 1373, 0x004e9794},   // ERROR_SPECIAL_USER
    { 1374, 0x004e97a8},   // ERROR_MEMBERS_PRIMARY_GROUP
    { 1375, 0x004e97c4},   // ERROR_TOKEN_ALREADY_IN_USE
    { 1376, 0x004e97e0},   // ERROR_NO_SUCH_ALIAS
    { 1377, 0x004e97f4},   // ERROR_MEMBER_NOT_IN_ALIAS
    { 1378, 0x004e9810},   // ERROR_MEMBER_IN_ALIAS
    { 1379, 0x004e9828},   // ERROR_ALIAS_EXISTS
    { 1380, 0x004e983c},   // ERROR_LOGON_NOT_GRANTED
    { 1381, 0x004e9854},   // ERROR_TOO_MANY_SECRETS
    { 1382, 0x004e986c},   // ERROR_SECRET_TOO_LONG
    { 1383, 0x004e9884},   // ERROR_INTERNAL_DB_ERROR
    { 1384, 0x004e989c},   // ERROR_TOO_MANY_CONTEXT_IDS
    { 1385, 0x004e98b8},   // ERROR_LOGON_TYPE_NOT_GRANTED
    { 1386, 0x004e98d8},   // ERROR_NT_CROSS_ENCRYPTION_REQUIRED
    { 1387, 0x004e98fc},   // ERROR_NO_SUCH_MEMBER
    { 1388, 0x004e9914},   // ERROR_INVALID_MEMBER
    { 1389, 0x004e992c},   // ERROR_TOO_MANY_SIDS
    { 1390, 0x004e9940},   // ERROR_LM_CROSS_ENCRYPTION_REQUIRED
    { 1391, 0x004e9964},   // ERROR_NO_INHERITANCE
    { 1392, 0x004e997c},   // ERROR_FILE_CORRUPT
    { 1393, 0x004e9990},   // ERROR_DISK_CORRUPT
    { 1394, 0x004e99a4},   // ERROR_NO_USER_SESSION_KEY
    { 1395, 0x004e99c0},   // ERROR_LICENSE_QUOTA_EXCEEDED
    { 1400, 0x004e99e0},   // ERROR_INVALID_WINDOW_HANDLE
    { 1401, 0x004e99fc},   // ERROR_INVALID_MENU_HANDLE
    { 1402, 0x004e9a18},   // ERROR_INVALID_CURSOR_HANDLE
    { 1403, 0x004e9a34},   // ERROR_INVALID_ACCEL_HANDLE
    { 1404, 0x004e9a50},   // ERROR_INVALID_HOOK_HANDLE
    { 1405, 0x004e9a6c},   // ERROR_INVALID_DWP_HANDLE
    { 1406, 0x004e9a88},   // ERROR_TLW_WITH_WSCHILD
    { 1407, 0x004e9aa0},   // ERROR_CANNOT_FIND_WND_CLASS
    { 1408, 0x004e9abc},   // ERROR_WINDOW_OF_OTHER_THREAD
    { 1409, 0x004e9adc},   // ERROR_HOTKEY_ALREADY_REGISTERED
    { 1410, 0x004e9afc},   // ERROR_CLASS_ALREADY_EXISTS
    { 1411, 0x004e9b18},   // ERROR_CLASS_DOES_NOT_EXIST
    { 1412, 0x004e9b34},   // ERROR_CLASS_HAS_WINDOWS
    { 1413, 0x004e9b4c},   // ERROR_INVALID_INDEX
    { 1414, 0x004e9b60},   // ERROR_INVALID_ICON_HANDLE
    { 1415, 0x004e9b7c},   // ERROR_PRIVATE_DIALOG_INDEX
    { 1416, 0x004e9b98},   // ERROR_LISTBOX_ID_NOT_FOUND
    { 1417, 0x004e9bb4},   // ERROR_NO_WILDCARD_CHARACTERS
    { 1418, 0x004e9bd4},   // ERROR_CLIPBOARD_NOT_OPEN
    { 1419, 0x004e9bf0},   // ERROR_HOTKEY_NOT_REGISTERED
    { 1420, 0x004e9c0c},   // ERROR_WINDOW_NOT_DIALOG
    { 1421, 0x004e9c24},   // ERROR_CONTROL_ID_NOT_FOUND
    { 1422, 0x004e9c40},   // ERROR_INVALID_COMBOBOX_MESSAGE
    { 1423, 0x004e9c60},   // ERROR_WINDOW_NOT_COMBOBOX
    { 1424, 0x004e9c7c},   // ERROR_INVALID_EDIT_HEIGHT
    { 1425, 0x004e9c98},   // ERROR_DC_NOT_FOUND
    { 1426, 0x004e9cac},   // ERROR_INVALID_HOOK_FILTER
    { 1427, 0x004e9cc8},   // ERROR_INVALID_FILTER_PROC
    { 1428, 0x004e9ce4},   // ERROR_HOOK_NEEDS_HMOD
    { 1429, 0x004e9cfc},   // ERROR_GLOBAL_ONLY_HOOK
    { 1430, 0x004e9d14},   // ERROR_JOURNAL_HOOK_SET
    { 1431, 0x004e9d2c},   // ERROR_HOOK_NOT_INSTALLED
    { 1432, 0x004e9d48},   // ERROR_INVALID_LB_MESSAGE
    { 1433, 0x004e9d64},   // ERROR_SETCOUNT_ON_BAD_LB
    { 1434, 0x004e9d80},   // ERROR_LB_WITHOUT_TABSTOPS
    { 1435, 0x004e9d9c},   // ERROR_DESTROY_OBJECT_OF_OTHER_THREAD
    { 1436, 0x004e9dc4},   // ERROR_CHILD_WINDOW_MENU
    { 1437, 0x004e9ddc},   // ERROR_NO_SYSTEM_MENU
    { 1438, 0x004e9df4},   // ERROR_INVALID_MSGBOX_STYLE
    { 1439, 0x004e9e10},   // ERROR_INVALID_SPI_VALUE
    { 1440, 0x004e9e28},   // ERROR_SCREEN_ALREADY_LOCKED
    { 1441, 0x004e9e44},   // ERROR_HWNDS_HAVE_DIFF_PARENT
    { 1442, 0x004e9e64},   // ERROR_NOT_CHILD_WINDOW
    { 1443, 0x004e9e7c},   // ERROR_INVALID_GW_COMMAND
    { 1444, 0x004e9e98},   // ERROR_INVALID_THREAD_ID
    { 1445, 0x004e9eb0},   // ERROR_NON_MDICHILD_WINDOW
    { 1446, 0x004e9ecc},   // ERROR_POPUP_ALREADY_ACTIVE
    { 1447, 0x004e9ee8},   // ERROR_NO_SCROLLBARS
    { 1448, 0x004e9efc},   // ERROR_INVALID_SCROLLBAR_RANGE
    { 1449, 0x004e9f1c},   // ERROR_INVALID_SHOWWIN_COMMAND
    { 1450, 0x004e9f3c},   // ERROR_NO_SYSTEM_RESOURCES
    { 1451, 0x004e9f58},   // ERROR_NONPAGED_SYSTEM_RESOURCES
    { 1452, 0x004e9f78},   // ERROR_PAGED_SYSTEM_RESOURCES
    { 1453, 0x004e9f98},   // ERROR_WORKING_SET_QUOTA
    { 1454, 0x004e9fb0},   // ERROR_PAGEFILE_QUOTA
    { 1455, 0x004e9fc8},   // ERROR_COMMITMENT_LIMIT
    { 1456, 0x004e9fe0},   // ERROR_MENU_ITEM_NOT_FOUND
    { 1500, 0x004e9ffc},   // ERROR_EVENTLOG_FILE_CORRUPT
    { 1501, 0x004ea018},   // ERROR_EVENTLOG_CANT_START
    { 1502, 0x004ea034},   // ERROR_LOG_FILE_FULL
    { 1503, 0x004ea048},   // ERROR_EVENTLOG_FILE_CHANGED
    { 1728, 0x004ea064},   // RPC_S_PROTOCOL_ERROR
    { 1766, 0x004ea07c},   // RPC_S_INTERNAL_ERROR
    { 1768, 0x004ea094},   // RPC_S_ADDRESS_ERROR
    { 1784, 0x004ea0a8},   // ERROR_INVALID_USER_BUFFER
    { 1785, 0x004ea0c4},   // ERROR_UNRECOGNIZED_MEDIA
    { 1786, 0x004ea0e0},   // ERROR_NO_TRUST_LSA_SECRET
    { 1787, 0x004ea0fc},   // ERROR_NO_TRUST_SAM_ACCOUNT
    { 1788, 0x004ea118},   // ERROR_TRUSTED_DOMAIN_FAILURE
    { 1789, 0x004ea138},   // ERROR_TRUSTED_RELATIONSHIP_FAILURE
    { 1790, 0x004ea15c},   // ERROR_TRUST_FAILURE
    { 1792, 0x004ea170},   // ERROR_NETLOGON_NOT_STARTED
    { 1793, 0x004ea18c},   // ERROR_ACCOUNT_EXPIRED
    { 1794, 0x004ea1a4},   // ERROR_REDIRECTOR_HAS_OPEN_HANDLES
    { 1795, 0x004ea1c8},   // ERROR_PRINTER_DRIVER_ALREADY_INSTALLED
    { 1796, 0x004ea1f0},   // ERROR_UNKNOWN_PORT
    { 1797, 0x004ea204},   // ERROR_UNKNOWN_PRINTER_DRIVER
    { 1798, 0x004ea224},   // ERROR_UNKNOWN_PRINTPROCESSOR
    { 1799, 0x004ea244},   // ERROR_INVALID_SEPARATOR_FILE
    { 1800, 0x004ea264},   // ERROR_INVALID_PRIORITY
    { 1801, 0x004ea27c},   // ERROR_INVALID_PRINTER_NAME
    { 1802, 0x004ea298},   // ERROR_PRINTER_ALREADY_EXISTS
    { 1803, 0x004ea2b8},   // ERROR_INVALID_PRINTER_COMMAND
    { 1804, 0x004ea2d8},   // ERROR_INVALID_DATATYPE
    { 1805, 0x004ea2f0},   // ERROR_INVALID_ENVIRONMENT
    { 1807, 0x004ea30c},   // ERROR_NOLOGON_INTERDOMAIN_TRUST_ACCOUNT
    { 1808, 0x004ea334},   // ERROR_NOLOGON_WORKSTATION_TRUST_ACCOUNT
    { 1809, 0x004ea35c},   // ERROR_NOLOGON_SERVER_TRUST_ACCOUNT
    { 1810, 0x004ea380},   // ERROR_DOMAIN_TRUST_INCONSISTENT
    { 1811, 0x004ea3a0},   // ERROR_SERVER_HAS_OPEN_HANDLES
    { 1812, 0x004ea3c0},   // ERROR_RESOURCE_DATA_NOT_FOUND
    { 1813, 0x004ea3e0},   // ERROR_RESOURCE_TYPE_NOT_FOUND
    { 1814, 0x004ea400},   // ERROR_RESOURCE_NAME_NOT_FOUND
    { 1815, 0x004ea420},   // ERROR_RESOURCE_LANG_NOT_FOUND
    { 1816, 0x004ea440},   // ERROR_NOT_ENOUGH_QUOTA
    { 1823, 0x004ea458},   // RPC_S_NOT_RPC_ERROR
    { 1825, 0x004ea46c},   // RPC_S_SEC_PKG_ERROR
    { 1901, 0x004ea480},   // ERROR_INVALID_TIME
    { 1902, 0x004ea494},   // ERROR_INVALID_FORM_NAME
    { 1903, 0x004ea4ac},   // ERROR_INVALID_FORM_SIZE
    { 1904, 0x004ea4c4},   // ERROR_ALREADY_WAITING
    { 1905, 0x004ea4dc},   // ERROR_PRINTER_DELETED
    { 1906, 0x004ea4f4},   // ERROR_INVALID_PRINTER_STATE
    { 1907, 0x004ea510},   // ERROR_PASSWORD_MUST_CHANGE
    { 1908, 0x004ea52c},   // ERROR_DOMAIN_CONTROLLER_NOT_FOUND
    { 1909, 0x004ea550},   // ERROR_ACCOUNT_LOCKED_OUT
    { 2000, 0x004ea58c},   // ERROR_INVALID_PIXEL_FORMAT
    { 2001, 0x004ea5a8},   // ERROR_BAD_DRIVER
    { 2002, 0x004ea5bc},   // ERROR_INVALID_WINDOW_STYLE
    { 2003, 0x004ea5d8},   // ERROR_METAFILE_NOT_SUPPORTED
    { 2004, 0x004ea5f8},   // ERROR_TRANSFORM_NOT_SUPPORTED
    { 2005, 0x004ea618},   // ERROR_CLIPPING_NOT_SUPPORTED
    { 2202, 0x004e8ad0},   // ERROR_BAD_USERNAME
    { 2250, 0x004e8ae4},   // ERROR_NOT_CONNECTED
    { 2401, 0x004e8af8},   // ERROR_OPEN_FILES
    { 2402, 0x004e8b0c},   // ERROR_ACTIVE_CONNECTIONS
    { 2404, 0x004e8b28},   // ERROR_DEVICE_IN_USE
    { 3000, 0x004ea638},   // ERROR_UNKNOWN_PRINT_MONITOR
    { 3001, 0x004ea654},   // ERROR_PRINTER_DRIVER_IN_USE
    { 3002, 0x004ea670},   // ERROR_SPOOL_FILE_NOT_FOUND
    { 3003, 0x004ea68c},   // ERROR_SPL_NO_STARTDOC
    { 3004, 0x004ea6a4},   // ERROR_SPL_NO_ADDJOB
    { 3005, 0x004ea6b8},   // ERROR_PRINT_PROCESSOR_ALREADY_INSTALLED
    { 3006, 0x004ea6e0},   // ERROR_PRINT_MONITOR_ALREADY_INSTALLED
    { 4000, 0x004ea708},   // ERROR_WINS_INTERNAL
    { 4001, 0x004ea71c},   // ERROR_CAN_NOT_DEL_LOCAL_WINS
    { 4002, 0x004ea73c},   // ERROR_STATIC_INIT
    { 4003, 0x004ea750},   // ERROR_INC_BACKUP
    { 4004, 0x004ea764},   // ERROR_FULL_BACKUP
    { 4005, 0x004ea778},   // ERROR_REC_NON_EXISTENT
    { 4006, 0x004ea790},   // ERROR_RPL_NOT_ALLOWED
    { 6118, 0x004ea56c},   // ERROR_NO_BROWSER_SERVERS_FOUND
};
static const char* __cdecl Win32GetErrorString_int_rw(int e) {
    int lo = 0, hi = (int)(sizeof k_err_names / sizeof *k_err_names) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (k_err_names[mid].code == e) return S(k_err_names[mid].str);
        if (k_err_names[mid].code < e) lo = mid + 1;
        else hi = mid - 1;
    }
    return S(0x004ea7a8);                                          // "<unknown>"
}
static void fp_err_string(Footprint& f, int) { f.pure = true; }
PORT_FN(0x00416080, "Win32GetErrorString(int)", Win32GetErrorString_int_rw, fp_err_string)

// Win32GetErrorString() (0x416070): the thread's last error's name
static const char* __cdecl Win32GetErrorString_rw() { return Win32GetErrorString_int_o((int)kGetLastError()); }
static void fp_last_error(Footprint& f) { f.replay_only = "it reads the thread's last error, which the check's own calls can change"; }
PORT_FN(0x00416070, "Win32GetErrorString()", Win32GetErrorString_rw, fp_last_error)
