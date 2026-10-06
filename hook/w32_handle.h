// w32_handle.h -- the stand-ins' handle model (relink stage R2a: the game's Windows imports implemented by the port).
//
// race.exe calls 190 Windows functions through its import table. In the GCC standalone build the port fills that table
// with its own stand-ins (hook/w32_*.cpp), written so the same code runs on Windows now and on Linux in R2b: a Win32
// HANDLE is an entry in this table, an object of one of the stand-ins' kinds (a file, a mapping, an event, a semaphore,
// a thread, a find-file listing, ...). CloseHandle, DuplicateHandle and WaitForSingleObject work on any of them, as
// Windows' do. The objects synchronise with standard C++ (std::mutex / std::condition_variable), not Win32.
//
// Handle values look like Windows': non-null multiples of 4 (0x1004, 0x1008, ...), never INVALID_HANDLE_VALUE (-1);
// the pseudo-handles GetCurrentProcess() = (HANDLE)-1 and GetCurrentThread() = (HANDLE)-2 are recognised by the kinds
// that take them. A closed handle's slot is reused only after every other free slot -- a stale handle in the game
// fails (ERROR_INVALID_HANDLE) rather than reaching a new object, as long as possible.
//
// Last error: one per thread (w32_set_last_error / w32_last_error), what GetLastError returns.
#pragma once
#include <stdint.h>
#include <memory>

namespace w32 {

typedef uint32_t Handle;                       // a HANDLE as the game holds it (32-bit)
const Handle INVALID = 0xFFFFFFFFu;            // INVALID_HANDLE_VALUE; also GetCurrentProcess()'s pseudo-handle
const Handle CURRENT_THREAD = 0xFFFFFFFEu;     // GetCurrentThread()'s pseudo-handle

// WaitForSingleObject's results and the infinite timeout, as Windows'
const uint32_t WAIT_OBJECT_0_ = 0, WAIT_ABANDONED_ = 0x80, WAIT_TIMEOUT_ = 0x102, WAIT_FAILED_ = 0xFFFFFFFFu;
const uint32_t INFINITE_ = 0xFFFFFFFFu;

enum class Kind { File, Mapping, Event, Semaphore, Mutex, Thread, FindFile, Console, Other };

struct Object {
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() {}
    const Kind kind;
    // WaitForSingleObject on this object: WAIT_OBJECT_0_ / WAIT_TIMEOUT_ / WAIT_ABANDONED_ (ms = INFINITE_ waits for
    // ever). The default: not waitable -- WAIT_FAILED_ with ERROR_INVALID_HANDLE.
    virtual uint32_t wait(uint32_t ms);
    // CloseHandle of the last handle to it (the object is destroyed after, when no one else holds it). Files flush,
    // views stay valid until unmapped, as Windows'.
    virtual void closed() {}
};

// a new handle for obj (shared: DuplicateHandle adds another handle to the same object). 0 if the table is full.
Handle add(std::shared_ptr<Object> obj);
// the object behind h (null for an unknown or closed handle, or a pseudo-handle); with a kind, null unless it's that kind
std::shared_ptr<Object> get(Handle h);
std::shared_ptr<Object> get(Handle h, Kind k);
template <typename T> std::shared_ptr<T> get_as(Handle h, Kind k) { return std::static_pointer_cast<T>(get(h, k)); }
// CloseHandle / FindClose: false (and ERROR_INVALID_HANDLE) for an unknown handle
bool close(Handle h);
// DuplicateHandle within this process: a second handle to the same object (0 if h is unknown)
Handle duplicate(Handle h);
// how many handles are open (diagnostics; logged at exit)
uint32_t open_count();

// the calling thread's last error (GetLastError / SetLastError)
void set_last_error(uint32_t e);
uint32_t last_error();

// Win32 error codes the stand-ins set (the values the game may test)
enum : uint32_t {
    ERR_SUCCESS = 0,
    ERR_FILE_NOT_FOUND = 2,
    ERR_PATH_NOT_FOUND = 3,
    ERR_TOO_MANY_OPEN_FILES = 4,
    ERR_ACCESS_DENIED = 5,
    ERR_INVALID_HANDLE = 6,
    ERR_NOT_ENOUGH_MEMORY = 8,
    ERR_NO_MORE_FILES = 18,
    ERR_SHARING_VIOLATION = 32,
    ERR_HANDLE_EOF = 38,
    ERR_NOT_SUPPORTED = 50,
    ERR_FILE_EXISTS = 80,
    ERR_INVALID_PARAMETER = 87,
    ERR_CALL_NOT_IMPLEMENTED = 120,
    ERR_INSUFFICIENT_BUFFER = 122,
    ERR_ALREADY_EXISTS = 183,
    ERR_NEGATIVE_SEEK = 131,
};

}  // namespace w32
