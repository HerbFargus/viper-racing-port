// w32_path.h -- the stand-ins' path layer (relink stage R2a): the game's Windows paths, and the few OS calls the file
// stand-ins (w32_file.cpp) need underneath, in one place.
//
// The game passes Windows paths: backslashes (or slashes), any case, drive letters, "." and "..", relative to the
// current directory. Every file stand-in turns its path into a FULL Windows path first, with full() -- Windows'
// GetFullPathName algorithm, written portably and checked against the real one (test/w32_file_test.cpp) -- and then
// into the path the OS opens, with resolve():
//   * Windows (now): the full path itself (the OS resolves it: case, 8.3 names, devices, UNC).
//   * Linux (R2b, designed here, the #else parts in w32_path.cpp): the install folder is drive C:'s root -- race.exe is
//     C:\race.exe, the current directory starts at C:\ -- and a full path C:\a\b is looked up component by component
//     under the install folder: an exact name first, else the first case-insensitive match in that directory (a short
//     8.3 name the listing gave matches too), else the name as written (a file about to be made). Backslash becomes
//     slash only there. Other drives, UNC paths (\\server\share) and devices (\\.\x) don't exist: the errors Windows
//     gives for a missing drive / network path / device. Paths never leave the install folder ("C:\.." is C:\).
// The current directory is Windows-form in both: Windows' own (getcwd) now; on Linux a string kept here (the host's
// chdir follows it, for the port's own relative opens).
//
// The OS calls underneath (namespace w32::os) are POSIX-like (open/read/write/lseek/close/mkdir/unlink/chmod/stat) on
// both, through the C runtime on Windows (its _doserrno is the exact Win32 error); where only the OS can say what
// Windows says -- a directory listing with its 8.3 names and times, exact attributes, file mappings, the console -- a
// small #ifdef _WIN32 part, with the POSIX part beside it.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace w32 {

// a FILETIME as a 64-bit count of 100 ns since 1601-01-01 (UTC)
typedef uint64_t FileTime;

// Win32 attribute bits (as WIN32_FIND_DATAA / GetFileAttributes report them)
enum : uint32_t {
    ATTR_READONLY = 0x1, ATTR_HIDDEN = 0x2, ATTR_SYSTEM = 0x4, ATTR_DIRECTORY = 0x10, ATTR_ARCHIVE = 0x20,
    ATTR_NORMAL = 0x80, ATTR_TEMPORARY = 0x100, ATTR_REPARSE_POINT = 0x400,
};

// more Win32 error codes the path layer and the file stand-ins set (w32_handle.h has the common ones)
enum : uint32_t {
    ERR_INVALID_FUNCTION = 1,
    ERR_WRITE_PROTECT = 19,
    ERR_NOT_READY = 21,
    ERR_DISK_FULL = 112,
    ERR_INVALID_NAME = 123,
    ERR_DIR_NOT_EMPTY = 145,
    ERR_BAD_NETPATH = 53,
    ERR_FILENAME_EXCED_RANGE = 206,
    ERR_DIRECTORY = 267,
    ERR_INVALID_ADDRESS = 487,
    ERR_FILE_INVALID = 1006,
    ERR_MAPPED_ALIGNMENT = 1132,
};

namespace path {

const size_t MAX_PATH_ = 260;

// GetFullPathNameA's result for `in`: true and the full path (and, in *part, the offset of its last component, or
// npos when it has none -- a root, a trailing backslash, a device), or false and the Win32 error (ERROR_INVALID_NAME
// for an empty or all-blank path, ERROR_FILENAME_EXCED_RANGE for one MAX_PATH or longer, in or out).
bool full(const char* in, std::string& out, size_t* part, uint32_t* err);

// the current directory, Windows-form: "C:\x\y" (a trailing backslash only on a root)
std::string cwd();
// SetCurrentDirectoryA: the directory must exist (else ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND / ERROR_DIRECTORY
// for a file / ERROR_INVALID_NAME); the spelling given is kept, as Windows keeps it
bool set_cwd(const char* in, uint32_t* err);

// a full Windows path's kind
enum Kind { DRIVE, UNC, DEVICE };

// a path as the stand-ins open it
struct Resolved {
    std::string full;         // the full Windows path (full())
    std::string host;         // what the OS opens
    Kind kind;
    bool parent_found;        // its folder exists (a missing file is ERROR_FILE_NOT_FOUND, else ERROR_PATH_NOT_FOUND)
};
// full() and then the host path. false: err (full()'s errors; ERROR_PATH_NOT_FOUND for a drive that isn't there,
// ERROR_BAD_NETPATH / ERROR_FILE_NOT_FOUND for a UNC path / device where they don't exist -- Linux).
bool resolve(const char* in, Resolved& r, uint32_t* err);

// the key two opens of one file share (the sharing check): the full path, case-folded
std::string key(const std::string& full);

// Windows' name order in a directory listing (NTFS: by the upper-cased name, ordinal): <0, 0, >0
int compare_names(const char* a, const char* b);
// FindFirstFileA's wildcard match of a name against the pattern's last component (the DOS rules: '*', '?', and
// "*.*" / "*." / ".?" as cmd.exe has always meant them), case-insensitive
bool match(const char* pattern, const char* name);
// is `name` a valid 8.3 name as it stands (Windows gives no alternate name then)?
bool is_short_name(const char* name);
// the 8.3 alternate name Windows makes for `name` the n-th time (n = 1: "AZZARO~1.CAR")
std::string make_short_name(const char* name, int n);

// GetDriveTypeA: the root's drive type (DRIVE_FIXED 3, DRIVE_NO_ROOT_DIR 1, ...)
uint32_t drive_type(const char* root);

// Linux: the install folder (drive C:'s root) and the start directory; Windows: unused
void set_root(const char* host_dir);

}  // namespace path

// ---- the OS calls under the stand-ins (Win32 errors out) ------------------------------------------------------------
namespace os {

// a file's or folder's facts, as a directory listing reports them
struct Info {
    std::string name;         // its name in its folder (listings)
    std::string short_name;   // its 8.3 alternate name, "" when its name is one (listings)
    uint32_t attrs;           // ATTR_*
    uint64_t size;
    FileTime created, accessed, written;
    uint32_t reserved0;       // WIN32_FIND_DATAA.dwReserved0 (a reparse point's tag)
};
// stat: false and ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND / ... when it isn't there
bool stat(const std::string& host, Info& info, uint32_t* err);
// every entry of a folder (with "." and ".." where the OS lists them), in the OS's order
bool list(const std::string& host_dir, std::vector<Info>& out, uint32_t* err);

// open flags (POSIX-like, mapped per OS). attrs: a new file's attributes (ATTR_*; Linux keeps the read-only bit).
// Windows opens through CreateFileA sharing everything (the stand-ins keep the sharing rules themselves) and hands the
// handle to the C runtime; neither read nor write: an open for attributes only.
enum : int { O_READ = 1, O_WRITE = 2, O_CREATE = 4, O_TRUNCATE = 8, O_EXCLUSIVE = 16 };
int open(const std::string& host, int flags, uint32_t attrs, uint32_t* err);   // -1: failed
bool close(int fd);
int64_t read(int fd, void* buf, uint32_t n, uint32_t* err);            // -1: failed
int64_t write(int fd, const void* buf, uint32_t n, uint32_t* err);
int64_t seek(int fd, int64_t pos, int whence, uint32_t* err);           // whence 0/1/2; the new position, -1: failed
int64_t size(int fd, uint32_t* err);
bool truncate(int fd, int64_t size, uint32_t* err);
bool flush(int fd, uint32_t* err);

bool mkdir(const std::string& host, uint32_t* err);
bool unlink(const std::string& host, uint32_t* err);
// the attributes a SetFileAttributesA asks for (Windows: all of them; Linux: the read-only bit)
bool set_attributes(const std::string& host, uint32_t attrs, uint32_t* err);
// a copied file's times: the source's last-write time
bool set_written(const std::string& host, FileTime written, uint32_t* err);
bool chdir(const std::string& host, uint32_t* err);

// the host's errno as a Win32 error (Linux; on Windows the C runtime's _doserrno is used where it has one).
// missing_parent: for ENOENT, whether the folder was missing (ERROR_PATH_NOT_FOUND) or just the file.
uint32_t error_from_errno(int e, bool missing_parent);

}  // namespace os
}  // namespace w32
