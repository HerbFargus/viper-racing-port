// w32_file.cpp -- the file, folder, listing, mapping and console stand-ins (see w32_file.h).
//
// A file is a FileObj in the handle table: a C runtime / POSIX descriptor, the access it was opened for, the sharing it
// allows, its key (the full path, case-folded). Windows' sharing rules are kept here, in the process, for every open the
// game makes: a second open that wants to read / write / delete what an earlier one doesn't share, or that won't share
// what an earlier one has, fails with ERROR_SHARING_VIOLATION, and so does deleting a file open without
// FILE_SHARE_DELETE -- on Linux, where the OS knows no sharing, exactly as on Windows (only opens by other processes
// aren't seen). FILE_FLAG_DELETE_ON_CLOSE deletes the file when the last handle to it closes.
//
// Measured on Windows 11 (test/w32_file_test.cpp) and kept: CreateFileA sets the last error on success (ERROR_ALREADY_
// EXISTS for an OPEN_ALWAYS / CREATE_ALWAYS that found the file, else 0); a read-only file can't be opened for writing,
// truncated or deleted (ERROR_ACCESS_DENIED) but can for DELETE alone; TRUNCATE_EXISTING needs GENERIC_WRITE or
// FILE_WRITE_DATA (else ERROR_INVALID_PARAMETER); CREATE_ALWAYS gives the file the attributes asked for and archive; a
// folder needs FILE_FLAG_BACKUP_SEMANTICS (else ERROR_ACCESS_DENIED, "folder\" ERROR_PATH_NOT_FOUND) and can then only
// be opened (CREATE_* ERROR_FILE_EXISTS, TRUNCATE_EXISTING ERROR_INVALID_PARAMETER); a path with a wildcard or < > " |
// is ERROR_INVALID_NAME; an open for attributes only takes no part in sharing; CloseHandle of a pseudo-handle succeeds
// and does nothing, of a listing fails (a listing isn't a kernel handle); CopyFileA shares its source's reads and
// deletes (writes too when a writer has it), its copy's reads and writes; FindFirstFileA's pattern is the full path's last
// component (plus a dot when the path ended in one: "*." lists the names with no extension), "" is ERROR_PATH_NOT_FOUND,
// a folder that is a file ERROR_DIRECTORY, a pattern ending in a backslash ERROR_INVALID_PARAMETER (relative) or
// ERROR_FILE_NOT_FOUND / ERROR_DIRECTORY / ERROR_PATH_NOT_FOUND; a failed call leaves the caller's WIN32_FIND_DATAA alone
// (dwReserved0/1 are left as Windows leaves them: whatever was on its stack -- here 0); GetTempFileNameA is
// <path>\<3 of prefix><hex unique>.tmp.
#define _FILE_OFFSET_BITS 64
#include "w32_file.h"
#include "w32_path.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <atomic>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#endif

namespace w32 {

std::shared_ptr<Object> (*current_thread_object)() = nullptr;
std::shared_ptr<Object> (*current_process_object)() = nullptr;

namespace {

// ---- Win32 constants ------------------------------------------------------------------------------------------------
enum : uint32_t {
    GENERIC_READ_ = 0x80000000u, GENERIC_WRITE_ = 0x40000000u, GENERIC_EXECUTE_ = 0x20000000u, GENERIC_ALL_ = 0x10000000u,
    FILE_READ_DATA_ = 1, FILE_WRITE_DATA_ = 2, FILE_APPEND_DATA_ = 4, FILE_EXECUTE_ = 0x20, DELETE_ = 0x10000,
    SHARE_READ = 1, SHARE_WRITE = 2, SHARE_DELETE = 4,
    CREATE_NEW_ = 1, CREATE_ALWAYS_ = 2, OPEN_EXISTING_ = 3, OPEN_ALWAYS_ = 4, TRUNCATE_EXISTING_ = 5,
    FLAG_DELETE_ON_CLOSE = 0x04000000, FLAG_BACKUP_SEMANTICS = 0x02000000,
    TYPE_UNKNOWN = 0, TYPE_DISK = 1, TYPE_CHAR = 2, TYPE_PIPE = 3,
    STD_INPUT = 0xFFFFFFF6u, STD_OUTPUT = 0xFFFFFFF5u, STD_ERROR = 0xFFFFFFF4u,
    DUP_CLOSE_SOURCE = 1,
    PAGE_READONLY_ = 2, PAGE_READWRITE_ = 4, PAGE_WRITECOPY_ = 8, PAGE_EXECUTE_READ_ = 0x20,
    PAGE_EXECUTE_READWRITE_ = 0x40, PAGE_EXECUTE_WRITECOPY_ = 0x80,
    MAP_COPY = 1, MAP_WRITE = 2, MAP_READ = 4, MAP_EXECUTE = 0x20,
    INVALID_SIZE = 0xFFFFFFFFu,
};
// the access bits the sharing rules look at
enum : uint32_t { A_READ = 1, A_WRITE = 2, A_DELETE = 4 };

uint32_t fail(uint32_t e) {
    set_last_error(e);
    return 0;
}
Handle fail_handle(uint32_t e) {
    set_last_error(e);
    return INVALID;
}

// ---- files ------------------------------------------------------------------------------------------------------------
struct FileObj;
std::mutex g_files;                                // the open files by key (the sharing rules), the mappings' names
std::multimap<std::string, FileObj*> g_open;
std::atomic<uint32_t> g_nfiles(0), g_nfinds(0), g_nmaps(0);

struct FileObj : Object {
    FileObj() : Object(Kind::File) {}
    ~FileObj() override;
    int fd = -1;               // a disk file's descriptor (Linux: also a standard stream's)
    void* os = nullptr;        // Windows: a standard stream's or console's own HANDLE
    bool std_stream = false;   // GetStdHandle's: never closed underneath
    bool dir = false;          // a folder (FILE_FLAG_BACKUP_SEMANTICS): no data
    uint32_t type = TYPE_DISK;
    uint32_t access = 0;       // A_*
    uint32_t share = 0;
    bool delete_on_close = false;
    std::string key, host;
    std::mutex mu;             // a position and the I/O at it
};

FileObj::~FileObj() {
    if (fd >= 0 && !std_stream) os::close(fd);
    if (key.empty()) return;
    bool last = true;
    std::lock_guard<std::mutex> lk(g_files);
    for (auto it = g_open.lower_bound(key); it != g_open.end() && it->first == key;) {
        if (it->second == this) {
            it = g_open.erase(it);
            continue;
        }
        last = false;
        if (delete_on_close) it->second->delete_on_close = true;   // the delete waits for the other opens
        ++it;
    }
    g_nfiles--;
    if (delete_on_close && last) {
        uint32_t e;
        os::unlink(host, &e);
    }
}

uint32_t access_bits(uint32_t access) {
    uint32_t a = 0;
    if (access & (GENERIC_READ_ | GENERIC_EXECUTE_ | GENERIC_ALL_ | FILE_READ_DATA_ | FILE_EXECUTE_)) a |= A_READ;
    if (access & (GENERIC_WRITE_ | GENERIC_ALL_ | FILE_WRITE_DATA_ | FILE_APPEND_DATA_)) a |= A_WRITE;
    if (access & (DELETE_ | GENERIC_ALL_)) a |= A_DELETE;
    return a;
}

// may an open wanting `access` and sharing `share` join the opens of key? (g_files held)
// (an open without read, write or delete access -- attributes only -- takes no part in the sharing)
bool share_ok(const std::string& key, uint32_t access, uint32_t share) {
    if (!(access & (A_READ | A_WRITE | A_DELETE))) return true;
    for (auto it = g_open.lower_bound(key); it != g_open.end() && it->first == key; ++it) {
        const FileObj* o = it->second;
        if (!(o->access & (A_READ | A_WRITE | A_DELETE))) continue;
        if ((access & A_READ) && !(o->share & SHARE_READ)) return false;
        if ((access & A_WRITE) && !(o->share & SHARE_WRITE)) return false;
        if ((access & A_DELETE) && !(o->share & SHARE_DELETE)) return false;
        if ((o->access & A_READ) && !(share & SHARE_READ)) return false;
        if ((o->access & A_WRITE) && !(share & SHARE_WRITE)) return false;
        if ((o->access & A_DELETE) && !(share & SHARE_DELETE)) return false;
    }
    return true;
}

// a path with a character no Windows file name has (the wildcards, < > " |) past its root
bool bad_name(const path::Resolved& r) {
    size_t from = r.kind == path::DRIVE ? 3 : r.kind == path::DEVICE ? 4 : 2;
    for (size_t i = from; i < r.full.size(); i++)
        if (strchr("*?<>\"|", r.full[i])) return true;
    return false;
}

#ifdef _WIN32
HANDLE os_handle(const FileObj& f) { return f.os ? (HANDLE)f.os : (HANDLE)_get_osfhandle(f.fd); }
uint32_t fd_type(int fd) { return GetFileType((HANDLE)_get_osfhandle(fd)); }
#else
uint32_t fd_type(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return TYPE_UNKNOWN;
    if (S_ISCHR(st.st_mode)) return TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return TYPE_PIPE;
    return TYPE_DISK;
}
#endif

// CreateFileA's work (CopyFileA opens through it too). INVALID and the error, or the new file.
std::shared_ptr<FileObj> open_file(const char* name, uint32_t access, uint32_t share, uint32_t disposition, uint32_t flags,
                                   uint32_t* err, bool* existed) {
    *existed = false;
    if (!name) {
        *err = ERR_PATH_NOT_FOUND;
        return nullptr;
    }
    if (disposition < CREATE_NEW_ || disposition > TRUNCATE_EXISTING_) {
        *err = ERR_INVALID_PARAMETER;
        return nullptr;
    }
    uint32_t acc = access_bits(access);
    if (disposition == TRUNCATE_EXISTING_ && !(access & (GENERIC_WRITE_ | GENERIC_ALL_ | FILE_WRITE_DATA_))) {
        *err = ERR_INVALID_PARAMETER;
        return nullptr;
    }
    if (flags & FLAG_DELETE_ON_CLOSE) acc |= A_DELETE;
    path::Resolved r;
    if (!path::resolve(name, r, err)) return nullptr;
    if (bad_name(r)) {
        *err = ERR_INVALID_NAME;
        return nullptr;
    }
    std::shared_ptr<FileObj> f = std::make_shared<FileObj>();
    f->access = acc;
    f->share = share & (SHARE_READ | SHARE_WRITE | SHARE_DELETE);
    f->delete_on_close = (flags & FLAG_DELETE_ON_CLOSE) != 0;
    f->host = r.host;
    std::lock_guard<std::mutex> lk(g_files);
    os::Info info;
    uint32_t serr = 0;
    const bool exists = r.kind == path::DRIVE && os::stat(r.host, info, &serr);
    *existed = exists;
    if (exists) {
        if (info.attrs & ATTR_DIRECTORY) {
            // a folder: only with FILE_FLAG_BACKUP_SEMANTICS, and only opened ("x\" without it: not found)
            if (!(flags & FLAG_BACKUP_SEMANTICS)) {
                *err = r.full.back() == '\\' ? ERR_PATH_NOT_FOUND : ERR_ACCESS_DENIED;
                return nullptr;
            }
            if (disposition == CREATE_NEW_ || disposition == CREATE_ALWAYS_) {
                *err = ERR_FILE_EXISTS;
                return nullptr;
            }
            if (disposition == TRUNCATE_EXISTING_) {
                *err = ERR_INVALID_PARAMETER;
                return nullptr;
            }
        } else if (disposition == CREATE_NEW_) {
            *err = ERR_FILE_EXISTS;
            return nullptr;
        }
        const std::string k = path::key(r.full);
        if (!share_ok(k, acc, f->share)) {
            *err = ERR_SHARING_VIOLATION;
            return nullptr;
        }
        const bool writes = (acc & A_WRITE) || (flags & FLAG_DELETE_ON_CLOSE) || disposition == CREATE_ALWAYS_ ||
                            disposition == TRUNCATE_EXISTING_;      // (DELETE access alone opens a read-only file)
        if ((info.attrs & ATTR_READONLY) && writes) {
            *err = ERR_ACCESS_DENIED;
            return nullptr;
        }
        if (disposition == CREATE_ALWAYS_ && (info.attrs & (ATTR_HIDDEN | ATTR_SYSTEM) & ~flags)) {
            *err = ERR_ACCESS_DENIED;        // (re-creating a hidden / system file must say so)
            return nullptr;
        }
        f->key = k;
        if (info.attrs & ATTR_DIRECTORY) {
            f->dir = true;                    // a folder handle: no data
            g_open.insert(std::make_pair(f->key, f.get()));
            g_nfiles++;
            return f;
        }
    } else {
        f->key = path::key(r.full);
    }
    int of = 0;
    if (acc & A_READ) of |= os::O_READ;
    if (acc & A_WRITE) of |= os::O_WRITE;
    // an existing file is opened as it is and emptied here (CREATE_ALWAYS / TRUNCATE_EXISTING): the C runtime's own
    // truncating open would refuse a hidden or system file
    const bool truncates = exists && (disposition == CREATE_ALWAYS_ || disposition == TRUNCATE_EXISTING_);
    if (!exists) {
        switch (disposition) {
        case CREATE_NEW_: of |= os::O_CREATE | os::O_EXCLUSIVE; break;
        case CREATE_ALWAYS_: case OPEN_ALWAYS_: of |= os::O_CREATE; break;
        default: break;
        }
    }
    if (truncates) of |= os::O_WRITE;
    // the attributes asked for: a new file is made with them (and archive)
    const uint32_t asked = flags & (ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE | ATTR_TEMPORARY | 0x1000 | 0x2000);
    f->fd = os::open(r.host, of, asked, err);
    if (f->fd < 0) {
        f->key.clear();
        return nullptr;
    }
    if (truncates && !os::truncate(f->fd, 0, err)) {
        f->key.clear();
        return nullptr;
    }
    // a file re-made (CREATE_ALWAYS) takes the attributes asked for and archive; a truncated one keeps its own
    if (exists && disposition == CREATE_ALWAYS_) {
        uint32_t e;
        os::set_attributes(r.host, asked | ATTR_ARCHIVE, &e);
    }
    f->type = r.kind == path::DRIVE ? TYPE_DISK : fd_type(f->fd);
    g_open.insert(std::make_pair(f->key, f.get()));
    g_nfiles++;
    return f;
}

std::shared_ptr<FileObj> file_of(Handle h) { return get_as<FileObj>(h, Kind::File); }

// ---- the standard handles ---------------------------------------------------------------------------------------------
std::mutex g_std_mu;
bool g_std_init;
Handle g_std[3];                   // what GetStdHandle returns: input, output, error

std::shared_ptr<FileObj> wrap_std(int i) {
    std::shared_ptr<FileObj> f = std::make_shared<FileObj>();
    f->std_stream = true;
    f->access = A_READ | A_WRITE;
    f->share = SHARE_READ | SHARE_WRITE;
#ifdef _WIN32
    HANDLE r = GetStdHandle((DWORD)(STD_INPUT - i));
    if (r == 0 || r == INVALID_HANDLE_VALUE) return nullptr;
    f->os = r;
    f->type = GetFileType(r);
#else
    if (fcntl(i, F_GETFD) == -1) return nullptr;
    f->fd = i;
    f->type = fd_type(i);
#endif
    return f;
}

void std_init_locked(bool force) {
    if (g_std_init && !force) return;
    g_std_init = true;
    for (int i = 0; i < 3; i++) {
        std::shared_ptr<FileObj> f = wrap_std(i);
#ifdef _WIN32
        if (!f) {
            g_std[i] = (Handle)(uintptr_t)GetStdHandle((DWORD)(STD_INPUT - i));   // 0 (none) or -1, as Windows has it
            continue;
        }
#else
        if (!f) {
            g_std[i] = 0;
            continue;
        }
#endif
        g_std[i] = add(f);
    }
}

#ifndef _WIN32
// Linux: a console is the terminal on the standard streams
bool is_console(const FileObj& f) { return f.std_stream && f.fd >= 0 && isatty(f.fd); }
std::string g_console_pending;      // ReadConsoleA: what's left of the line read
std::mutex g_console_mu;
#endif

// ---- listings -------------------------------------------------------------------------------------------------------
struct FindObj : Object {
    FindObj() : Object(Kind::FindFile) { g_nfinds++; }
    ~FindObj() override { g_nfinds--; }
    std::vector<os::Info> entries;
    size_t next = 0;
    std::mutex mu;
};

void fill_find(FindDataA* fd, const os::Info& i) {
    fd->dwFileAttributes = i.attrs;
    fd->ftCreationTime[0] = (uint32_t)i.created;
    fd->ftCreationTime[1] = (uint32_t)(i.created >> 32);
    fd->ftLastAccessTime[0] = (uint32_t)i.accessed;
    fd->ftLastAccessTime[1] = (uint32_t)(i.accessed >> 32);
    fd->ftLastWriteTime[0] = (uint32_t)i.written;
    fd->ftLastWriteTime[1] = (uint32_t)(i.written >> 32);
    fd->nFileSizeHigh = (uint32_t)(i.size >> 32);
    fd->nFileSizeLow = (uint32_t)i.size;
    fd->dwReserved0 = i.reserved0;
    fd->dwReserved1 = 0;
    const size_t n = std::min(i.name.size(), sizeof fd->cFileName - 1);
    memcpy(fd->cFileName, i.name.c_str(), n);
    fd->cFileName[n] = 0;
    const size_t s = std::min(i.short_name.size(), sizeof fd->cAlternateFileName - 1);
    memcpy(fd->cAlternateFileName, i.short_name.c_str(), s);
    fd->cAlternateFileName[s] = 0;
}

// ---- mappings -------------------------------------------------------------------------------------------------------
struct MapObj : Object {
    MapObj() : Object(Kind::Mapping) { g_nmaps++; }
    ~MapObj() override;
    uint64_t size = 0;
    uint32_t protect = 0;
    std::string name;
#ifdef _WIN32
    HANDLE section = 0;
#else
    int fd = -1;               // a dup of the file's (-1: anonymous)
#endif
};
std::map<std::string, std::weak_ptr<MapObj>> g_map_names;
struct View {
    uint32_t size;
    std::shared_ptr<MapObj> map;
};
std::map<uintptr_t, View> g_views;

MapObj::~MapObj() {
#ifdef _WIN32
    if (section) CloseHandle(section);
#else
    if (fd >= 0) ::close(fd);
#endif
    g_nmaps--;
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(g_files);
    auto it = g_map_names.find(name);
    if (it != g_map_names.end() && it->second.expired()) g_map_names.erase(it);
}

}  // namespace

uint32_t file_open_count() { return g_nfiles + g_nfinds + g_nmaps; }

}  // namespace w32

using namespace w32;

// =====================================================================================================================
// files
// =====================================================================================================================
w32_HANDLE W32_STDCALL w32_CreateFileA(const char* name, uint32_t access, uint32_t share, void*, uint32_t disposition,
                                       uint32_t flags, w32_HANDLE) {
    uint32_t err = 0;
    bool existed;
    std::shared_ptr<FileObj> f = open_file(name, access, share, disposition, flags, &err, &existed);
    if (!f) return fail_handle(err);
    const Handle h = add(f);
    if (!h) return fail_handle(ERR_TOO_MANY_OPEN_FILES);
    set_last_error(existed && (disposition == OPEN_ALWAYS_ || disposition == CREATE_ALWAYS_) ? (uint32_t)ERR_ALREADY_EXISTS : 0u);
    return h;
}

uint32_t W32_STDCALL w32_ReadFile(w32_HANDLE h, void* buf, uint32_t n, uint32_t* read, Overlapped* ov) {
    if (read) *read = 0;
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    if (f->os) {
        DWORD got = 0;
        if (!ReadFile((HANDLE)f->os, buf, n, &got, 0)) return fail(GetLastError());
        if (read) *read = got;
        return 1;
    }
#endif
    if (!(f->access & A_READ)) return fail(ERR_ACCESS_DENIED);
    if (f->dir) return fail(ERR_INVALID_FUNCTION);
    std::lock_guard<std::mutex> lk(f->mu);
    uint32_t err = 0;
    if (ov && f->type == TYPE_DISK && os::seek(f->fd, ((int64_t)ov->OffsetHigh << 32) | ov->Offset, 0, &err) < 0)
        return fail(err);
    uint32_t got = 0;
    while (got < n) {
        const int64_t r = os::read(f->fd, (char*)buf + got, n - got, &err);
        if (r < 0) {
            if (read) *read = got;
            return fail(err);
        }
        if (r == 0) break;
        got += (uint32_t)r;
        if (f->type != TYPE_DISK) break;           // (a terminal or pipe: what's there)
    }
    if (read) *read = got;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = got;
    }
    return 1;
}

uint32_t W32_STDCALL w32_WriteFile(w32_HANDLE h, const void* buf, uint32_t n, uint32_t* written, Overlapped* ov) {
    if (written) *written = 0;
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    if (f->os) {
        DWORD put = 0;
        if (!WriteFile((HANDLE)f->os, buf, n, &put, 0)) return fail(GetLastError());
        if (written) *written = put;
        return 1;
    }
#endif
    if (!(f->access & A_WRITE)) return fail(ERR_ACCESS_DENIED);
    if (f->dir) return fail(ERR_INVALID_FUNCTION);
    std::lock_guard<std::mutex> lk(f->mu);
    uint32_t err = 0;
    if (ov && f->type == TYPE_DISK && os::seek(f->fd, ((int64_t)ov->OffsetHigh << 32) | ov->Offset, 0, &err) < 0)
        return fail(err);
    uint32_t put = 0;
    while (put < n) {
        const int64_t r = os::write(f->fd, (const char*)buf + put, n - put, &err);
        if (r < 0) {
            if (written) *written = put;
            return fail(err);
        }
        if (r == 0) {
            if (written) *written = put;
            return fail(ERR_DISK_FULL);
        }
        put += (uint32_t)r;
    }
    if (written) *written = put;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = put;
    }
    return 1;
}

uint32_t W32_STDCALL w32_SetFilePointer(w32_HANDLE h, int32_t distance, int32_t* distance_high, uint32_t method) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail_handle(ERR_INVALID_HANDLE);
#ifdef _WIN32
    if (f->os) {
        const DWORD r = SetFilePointer((HANDLE)f->os, distance, (PLONG)distance_high, method);
        if (r == INVALID_SET_FILE_POINTER) {
            const DWORD e = GetLastError();
            if (e) set_last_error(e);
        }
        return r;
    }
#endif
    if (method > 2) return fail_handle(ERR_INVALID_PARAMETER);
    if (f->dir) return fail_handle(ERR_INVALID_FUNCTION);
    const int64_t d = distance_high ? (int64_t)(((uint64_t)(uint32_t)*distance_high << 32) | (uint32_t)distance)
                                    : (int64_t)distance;
    std::lock_guard<std::mutex> lk(f->mu);
    uint32_t err = 0;
    int64_t base = 0;
    if (method == 1) base = os::seek(f->fd, 0, 1, &err);
    else if (method == 2) base = os::size(f->fd, &err);
    if (base < 0) return fail_handle(err);
    const int64_t np = base + d;
    if (np < 0) return fail_handle(ERR_NEGATIVE_SEEK);
    if (!distance_high && (uint64_t)np > 0xFFFFFFFFull) return fail_handle(ERR_INVALID_PARAMETER);
    if (os::seek(f->fd, np, 0, &err) < 0) return fail_handle(err);
    if (distance_high) *distance_high = (int32_t)(np >> 32);
    if ((uint32_t)np == INVALID_SIZE) set_last_error(0);   // (the caller tells success from INVALID_SET_FILE_POINTER)
    return (uint32_t)np;
}

uint32_t W32_STDCALL w32_SetEndOfFile(w32_HANDLE h) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    if (f->os) return SetEndOfFile((HANDLE)f->os) ? 1 : fail(GetLastError());
#endif
    if (!(f->access & A_WRITE)) return fail(ERR_ACCESS_DENIED);
    if (f->dir) return fail(ERR_INVALID_FUNCTION);
    std::lock_guard<std::mutex> lk(f->mu);
    uint32_t err = 0;
    const int64_t pos = os::seek(f->fd, 0, 1, &err);
    if (pos < 0 || !os::truncate(f->fd, pos, &err)) return fail(err);
    return 1;
}

uint32_t W32_STDCALL w32_FlushFileBuffers(w32_HANDLE h) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    if (f->os) return FlushFileBuffers((HANDLE)f->os) ? 1 : fail(GetLastError());
#endif
    if (!(f->access & A_WRITE)) return fail(ERR_ACCESS_DENIED);
    if (f->dir) return fail(ERR_INVALID_FUNCTION);
    uint32_t err = 0;
    if (!os::flush(f->fd, &err)) return fail(err);
    return 1;
}

uint32_t W32_STDCALL w32_GetFileSize(w32_HANDLE h, uint32_t* high) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail_handle(ERR_INVALID_HANDLE);
#ifdef _WIN32
    if (f->os) {
        const DWORD r = GetFileSize((HANDLE)f->os, (LPDWORD)high);
        if (r == INVALID_FILE_SIZE) set_last_error(GetLastError());
        return r;
    }
#endif
    if (f->dir) {
        if (high) *high = 0;
        return 0;
    }
    uint32_t err = 0;
    const int64_t s = os::size(f->fd, &err);
    if (s < 0) return fail_handle(err);
    if (high) *high = (uint32_t)((uint64_t)s >> 32);
    if ((uint32_t)s == INVALID_SIZE) set_last_error(0);
    return (uint32_t)s;
}

uint32_t W32_STDCALL w32_GetFileType(w32_HANDLE h) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
    return f->type;
}

w32_HANDLE W32_STDCALL w32_GetStdHandle(uint32_t which) {
    if (which != STD_INPUT && which != STD_OUTPUT && which != STD_ERROR) return fail_handle(ERR_INVALID_HANDLE);
    std::lock_guard<std::mutex> lk(g_std_mu);
    std_init_locked(false);
    return g_std[STD_INPUT - which];
}

uint32_t W32_STDCALL w32_SetStdHandle(uint32_t which, w32_HANDLE h) {
    if (which != STD_INPUT && which != STD_OUTPUT && which != STD_ERROR) return fail(ERR_INVALID_HANDLE);
    std::lock_guard<std::mutex> lk(g_std_mu);
    std_init_locked(false);
    g_std[STD_INPUT - which] = h;                  // (just the value, as Windows keeps it)
    return 1;
}

uint32_t W32_STDCALL w32_CloseHandle(w32_HANDLE h) {
    if (h == INVALID || h == CURRENT_THREAD) return 1;   // the pseudo-handles: nothing to close
    std::shared_ptr<Object> o = get(h);
    if (!o || o->kind == Kind::FindFile) return fail(ERR_INVALID_HANDLE);   // (a listing isn't a kernel handle)
    o.reset();
    return close(h) ? 1 : 0;
}

uint32_t W32_STDCALL w32_DuplicateHandle(w32_HANDLE src_process, w32_HANDLE src, w32_HANDLE dst_process,
                                         w32_HANDLE* dst, uint32_t, uint32_t, uint32_t options) {
    // within this process only (its pseudo-handle, or a handle to it)
    auto is_self = [](Handle p) {
        if (p == INVALID) return true;
        std::shared_ptr<Object> o = get(p);
        return o && current_process_object && o == current_process_object();
    };
    std::shared_ptr<Object> o;
    if (!is_self(src_process) || !is_self(dst_process)) {
        if (dst) *dst = 0;
        return fail(ERR_INVALID_HANDLE);
    }
    if (src == CURRENT_THREAD) o = current_thread_object ? current_thread_object() : nullptr;
    else if (src == INVALID) o = current_process_object ? current_process_object() : nullptr;
    else o = get(src);
    if (!o || o->kind == Kind::FindFile) {
        if (dst) *dst = 0;
        if (options & DUP_CLOSE_SOURCE) w32_CloseHandle(src);
        return fail(ERR_INVALID_HANDLE);
    }
    Handle n = 0;
    if (dst) {
        n = add(o);
        if (!n) return fail(ERR_TOO_MANY_OPEN_FILES);
        *dst = n;
    }
    o.reset();
    if (options & DUP_CLOSE_SOURCE) close(src);
    return 1;
}

uint32_t W32_STDCALL w32_DeleteFileA(const char* name) {
    uint32_t err = 0;
    path::Resolved r;
    if (!path::resolve(name, r, &err)) return fail(err);
    if (bad_name(r)) return fail(ERR_INVALID_NAME);
    std::lock_guard<std::mutex> lk(g_files);
    os::Info info;
    if (!os::stat(r.host, info, &err)) return fail(err);
    if (info.attrs & (ATTR_DIRECTORY | ATTR_READONLY)) return fail(ERR_ACCESS_DENIED);
    if (!share_ok(path::key(r.full), A_DELETE, SHARE_READ | SHARE_WRITE | SHARE_DELETE)) return fail(ERR_SHARING_VIOLATION);
    if (!os::unlink(r.host, &err)) return fail(err);
    return 1;
}

uint32_t W32_STDCALL w32_CopyFileA(const char* from, const char* to, uint32_t fail_if_exists) {
    // the source opened to read sharing reads and deletes (again sharing writes too if a writer has it open), the copy
    // made to write sharing reads and writes -- the sharing Windows' CopyFileA was measured to have: a copy onto its
    // own source is a sharing violation -- the data, then the source's attributes (and the archive bit) and
    // last-write time on the copy
    uint32_t err = 0;
    bool existed;
    std::shared_ptr<FileObj> a = open_file(from, GENERIC_READ_, SHARE_READ | SHARE_DELETE, OPEN_EXISTING_, 0, &err, &existed);
    if (!a && err == ERR_SHARING_VIOLATION)
        a = open_file(from, GENERIC_READ_, SHARE_READ | SHARE_WRITE | SHARE_DELETE, OPEN_EXISTING_, 0, &err, &existed);
    if (!a) return fail(err);
    if (a->dir) return fail(ERR_ACCESS_DENIED);
    os::Info info;
    if (!os::stat(a->host, info, &err)) return fail(err);
    std::shared_ptr<FileObj> b = open_file(to, GENERIC_WRITE_, SHARE_READ | SHARE_WRITE,
                                           fail_if_exists ? CREATE_NEW_ : CREATE_ALWAYS_,
                                           info.attrs & (ATTR_HIDDEN | ATTR_SYSTEM), &err, &existed);
    if (!b) return fail(err);
    std::vector<char> buf(0x10000);
    for (;;) {
        const int64_t n = os::read(a->fd, buf.data(), (uint32_t)buf.size(), &err);
        if (n < 0) return fail(err);
        if (n == 0) break;
        for (int64_t put = 0; put < n;) {
            const int64_t w = os::write(b->fd, buf.data() + put, (uint32_t)(n - put), &err);
            if (w <= 0) return fail(w < 0 ? err : ERR_DISK_FULL);
            put += w;
        }
    }
    const std::string host = b->host;
    a.reset();
    b.reset();
    if (!os::set_written(host, info.written, &err)) return fail(err);
    if (!os::set_attributes(host, (info.attrs & ~(ATTR_DIRECTORY | ATTR_NORMAL)) | ATTR_ARCHIVE, &err)) return fail(err);
    set_last_error(0);
    return 1;
}

uint32_t W32_STDCALL w32_CreateDirectoryA(const char* name, void*) {
    uint32_t err = 0;
    path::Resolved r;
    if (!path::resolve(name, r, &err)) return fail(err);
    if (bad_name(r)) return fail(ERR_INVALID_NAME);
    if (!os::mkdir(r.host, &err)) return fail(err);
    return 1;
}

uint32_t W32_STDCALL w32_SetFileAttributesA(const char* name, uint32_t attrs) {
    uint32_t err = 0;
    path::Resolved r;
    if (!path::resolve(name, r, &err)) return fail(err);
    if (bad_name(r)) return fail(ERR_INVALID_NAME);
    if (!os::set_attributes(r.host, attrs, &err)) return fail(err);
    return 1;
}

// =====================================================================================================================
// paths
// =====================================================================================================================
uint32_t W32_STDCALL w32_GetFullPathNameA(const char* name, uint32_t n, char* buf, char** file_part) {
    std::string f;
    size_t part;
    uint32_t err = 0;
    if (!path::full(name, f, &part, &err)) return fail(err);
    if (n <= f.size()) return (uint32_t)f.size() + 1;
    memcpy(buf, f.c_str(), f.size() + 1);
    if (file_part) *file_part = part == std::string::npos ? nullptr : buf + part;
    return (uint32_t)f.size();
}

uint32_t W32_STDCALL w32_GetCurrentDirectoryA(uint32_t n, char* buf) {
    const std::string c = path::cwd();
    if (n <= c.size()) return (uint32_t)c.size() + 1;
    memcpy(buf, c.c_str(), c.size() + 1);
    return (uint32_t)c.size();
}

uint32_t W32_STDCALL w32_SetCurrentDirectoryA(const char* name) {
    uint32_t err = 0;
    if (!path::set_cwd(name, &err)) return fail(err);
    return 1;
}

uint32_t W32_STDCALL w32_GetDriveTypeA(const char* root) { return path::drive_type(root); }

uint32_t W32_STDCALL w32_GetTempFileNameA(const char* dir, const char* prefix, uint32_t unique, char* buf) {
    // <dir>\<up to 3 of prefix><unique, hex>.tmp; unique 0: a free number made into an empty file
    uint32_t err = 0;
    std::string base = dir ? dir : "";
    if (!base.empty()) {
        path::Resolved r;
        os::Info info;
        if (!path::resolve(base.c_str(), r, &err) || !os::stat(r.host, info, &err) || !(info.attrs & ATTR_DIRECTORY))
            return fail(ERR_DIRECTORY);
    }
    if (base.empty() || (base.back() != '\\' && base.back() != '/')) base += '\\';
    std::string pre = prefix ? std::string(prefix).substr(0, 3) : std::string();
    unique &= 0xFFFF;
    char num[16];
    if (unique) {
        snprintf(num, sizeof num, "%X", unique);
        const std::string name = base + pre + num + ".tmp";
        if (name.size() >= path::MAX_PATH_) return fail(ERR_FILENAME_EXCED_RANGE);
        memcpy(buf, name.c_str(), name.size() + 1);
        if (base == "\\") set_last_error(ERR_PATH_NOT_FOUND);   // (Windows' own: dir "" is looked up and missed)
        return unique;
    }
#ifdef _WIN32
    uint32_t start = GetTickCount() & 0xFFFF;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t start = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) & 0xFFFF;
#endif
    if (!start) start = 1;
    for (uint32_t k = 0; k < 0xFFFF; k++) {
        uint32_t u = (start + k) & 0xFFFF;
        if (!u) continue;
        snprintf(num, sizeof num, "%X", u);
        const std::string name = base + pre + num + ".tmp";
        bool existed;
        std::shared_ptr<FileObj> f = open_file(name.c_str(), GENERIC_WRITE_, 0, CREATE_NEW_, ATTR_NORMAL, &err, &existed);
        if (f) {
            f.reset();
            memcpy(buf, name.c_str(), name.size() + 1);
            set_last_error(0);
            return u;
        }
        if (err != ERR_FILE_EXISTS && err != ERR_ALREADY_EXISTS) return fail(err);
    }
    return fail(ERR_FILE_EXISTS);
}

// =====================================================================================================================
// listings
// =====================================================================================================================
w32_HANDLE W32_STDCALL w32_FindFirstFileA(const char* pattern, FindDataA* fd) {
    if (!pattern || !*pattern) return fail_handle(ERR_PATH_NOT_FOUND);
    uint32_t err = 0;
    std::string full;
    if (!path::full(pattern, full, 0, &err)) return fail_handle(err);
    if (full.size() >= 4 && full[0] == '\\' && full[1] == '\\' && (full[2] == '.' || full[2] == '?'))
        return fail_handle(ERR_INVALID_NAME);
    // the folder and the pattern (the full path's last component; a dot the full path dropped from the end comes back)
    const size_t s = full.rfind('\\');
    std::string pat = full.substr(s + 1);
    std::string dir = full.substr(0, s);
    if (dir.size() == 2 && dir[1] == ':') dir += '\\';
    if (pat.empty()) {
        // a path ending in a backslash: relative, ERROR_INVALID_PARAMETER; else a folder there (nothing to match)
        // ERROR_FILE_NOT_FOUND, a file ERROR_DIRECTORY, nothing ERROR_PATH_NOT_FOUND
        const bool relative = !(pattern[0] == '\\' || pattern[0] == '/') && !(pattern[0] && pattern[1] == ':');
        if (relative) return fail_handle(ERR_INVALID_PARAMETER);
        path::Resolved rd;
        os::Info info;
        if (!path::resolve(dir.c_str(), rd, &err) || !os::stat(rd.host, info, &err)) return fail_handle(ERR_PATH_NOT_FOUND);
        return fail_handle(info.attrs & ATTR_DIRECTORY ? (uint32_t)ERR_FILE_NOT_FOUND : (uint32_t)ERR_DIRECTORY);
    }
    {
        const char* last = pattern;
        for (const char* p = pattern; *p; p++)
            if (*p == '\\' || *p == '/') last = p + 1;
        const size_t n = strlen(last);
        if (n && last[n - 1] == '.' && strcmp(last, ".") && strcmp(last, "..")) pat += '.';
    }
    path::Resolved r;
    if (!path::resolve(dir.c_str(), r, &err)) return fail_handle(err);
    std::vector<os::Info> all;
    if (!os::list(r.host, all, &err)) {
        os::Info info;
        uint32_t e2;
        if (err == ERR_PATH_NOT_FOUND && os::stat(r.host, info, &e2) && !(info.attrs & ATTR_DIRECTORY))
            err = ERR_DIRECTORY;
        return fail_handle(err);
    }
    // in the folder's order: "." and ".." first, then each name and each short name, sorted; a file is listed at its
    // long name if that matches, else at its short name if that does
    std::shared_ptr<FindObj> f = std::make_shared<FindObj>();
    const bool root = dir.size() == 3 && dir[1] == ':';
    struct Key {
        const char* name;
        size_t entry;
        bool is_short;
    };
    std::vector<Key> keys;
    for (size_t i = 0; i < all.size(); i++) {
        const os::Info& e = all[i];
        if (e.name == "." || e.name == "..") {
            if (!root && path::match(pat.c_str(), e.name.c_str())) f->entries.push_back(e);
            continue;
        }
        keys.push_back(Key{e.name.c_str(), i, false});
        if (!e.short_name.empty()) keys.push_back(Key{e.short_name.c_str(), i, true});
    }
    std::stable_sort(keys.begin(), keys.end(),
                     [](const Key& a, const Key& b) { return path::compare_names(a.name, b.name) < 0; });
    for (const Key& k : keys) {
        const os::Info& e = all[k.entry];
        if (!k.is_short) {
            if (path::match(pat.c_str(), e.name.c_str())) f->entries.push_back(e);
        } else if (path::match(pat.c_str(), e.short_name.c_str()) && !path::match(pat.c_str(), e.name.c_str())) {
            f->entries.push_back(e);
        }
    }
    if (f->entries.empty()) return fail_handle(ERR_FILE_NOT_FOUND);
    fill_find(fd, f->entries[0]);
    f->next = 1;
    const Handle h = add(f);
    if (!h) return fail_handle(ERR_NOT_ENOUGH_MEMORY);
    return h;
}

uint32_t W32_STDCALL w32_FindNextFileA(w32_HANDLE h, FindDataA* fd) {
    std::shared_ptr<FindObj> f = get_as<FindObj>(h, Kind::FindFile);
    if (!f) return fail(ERR_INVALID_HANDLE);
    std::lock_guard<std::mutex> lk(f->mu);
    if (f->next >= f->entries.size()) return fail(ERR_NO_MORE_FILES);
    fill_find(fd, f->entries[f->next++]);
    return 1;
}

uint32_t W32_STDCALL w32_FindClose(w32_HANDLE h) {
    if (!get(h, Kind::FindFile)) return fail(ERR_INVALID_HANDLE);
    return close(h) ? 1 : 0;
}

// =====================================================================================================================
// mappings
// =====================================================================================================================
w32_HANDLE W32_STDCALL w32_CreateFileMappingA(w32_HANDLE file, void*, uint32_t protect, uint32_t size_high,
                                              uint32_t size_low, const char* name) {
    const uint32_t prot = protect & 0xFF;
    if (prot != PAGE_READONLY_ && prot != PAGE_READWRITE_ && prot != PAGE_WRITECOPY_ && prot != PAGE_EXECUTE_READ_ &&
        prot != PAGE_EXECUTE_READWRITE_ && prot != PAGE_EXECUTE_WRITECOPY_)
        return fail(ERR_INVALID_PARAMETER);
    const bool writes = prot == PAGE_READWRITE_ || prot == PAGE_EXECUTE_READWRITE_;
    if (name && *name) {                           // a mapping of this name already: another handle to it
        std::shared_ptr<MapObj> m;
        {
            std::lock_guard<std::mutex> lk(g_files);
            auto it = g_map_names.find(name);
            if (it != g_map_names.end()) m = it->second.lock();
        }
        if (m) {
            const Handle h = add(m);
            if (!h) return fail(ERR_NOT_ENOUGH_MEMORY);
            set_last_error(ERR_ALREADY_EXISTS);
            return h;
        }
    }
    uint64_t size = ((uint64_t)size_high << 32) | size_low;
    std::shared_ptr<MapObj> m = std::make_shared<MapObj>();
    m->protect = prot;
    if (file == INVALID) {
        if (!size) return fail(ERR_INVALID_PARAMETER);
#ifdef _WIN32
        m->section = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, protect, size_high, size_low, 0);
        if (!m->section) return fail(GetLastError());
#endif
    } else {
        std::shared_ptr<FileObj> f = file_of(file);
        if (!f || f->dir) return fail(ERR_INVALID_HANDLE);
        if (!(f->access & A_READ) || (writes && !(f->access & A_WRITE))) return fail(ERR_ACCESS_DENIED);
        uint32_t err = 0;
        const int64_t fs = os::size(f->fd, &err);
        if (fs < 0) return fail(err);
        if (!size) {
            if (!fs) return fail(ERR_FILE_INVALID);
            size = (uint64_t)fs;
        } else if (size > (uint64_t)fs) {
            if (!writes) return fail(ERR_NOT_ENOUGH_MEMORY);
            if (!os::truncate(f->fd, (int64_t)size, &err)) return fail(err);   // (the file grows to the mapping)
        }
#ifdef _WIN32
        m->section = CreateFileMappingA(os_handle(*f), 0, protect, size_high, size_low, 0);
        if (!m->section) return fail(GetLastError());
#else
        m->fd = dup(f->fd);
        if (m->fd < 0) return fail(os::error_from_errno(errno, false));
#endif
    }
    m->size = size;
    if (name && *name) {
        m->name = name;
        std::lock_guard<std::mutex> lk(g_files);
        g_map_names[name] = m;
    }
    const Handle h = add(m);
    if (!h) return fail(ERR_NOT_ENOUGH_MEMORY);
    set_last_error(0);
    return h;
}

void* W32_STDCALL w32_MapViewOfFile(w32_HANDLE mapping, uint32_t access, uint32_t offset_high, uint32_t offset_low,
                                    uint32_t n) {
    std::shared_ptr<MapObj> m = get_as<MapObj>(mapping, Kind::Mapping);
    if (!m) {
        set_last_error(ERR_INVALID_HANDLE);
        return nullptr;
    }
    const bool ro = m->protect == PAGE_READONLY_ || m->protect == PAGE_EXECUTE_READ_;
    const bool cow = (access & MAP_COPY) && access != 0xF001F;
    if ((access & MAP_WRITE) && !cow && ro) {
        set_last_error(ERR_ACCESS_DENIED);
        return nullptr;
    }
    const uint64_t off = ((uint64_t)offset_high << 32) | offset_low;
    if (off & 0xFFFF) {
        set_last_error(ERR_MAPPED_ALIGNMENT);
        return nullptr;
    }
    if (off > m->size || (n && off + n > m->size)) {
        set_last_error(ERR_ACCESS_DENIED);
        return nullptr;
    }
    const uint64_t len = n ? n : m->size - off;
    void* p;
#ifdef _WIN32
    p = MapViewOfFile(m->section, access, offset_high, offset_low, n);
    if (!p) {
        set_last_error(GetLastError());
        return nullptr;
    }
#else
    int prot = PROT_READ;
    if ((access & MAP_WRITE) || cow) prot |= PROT_WRITE;
    if (access & MAP_EXECUTE) prot |= PROT_EXEC;
    const int flags = (cow ? MAP_PRIVATE : MAP_SHARED) | (m->fd < 0 ? MAP_ANONYMOUS : 0);
    p = mmap(0, (size_t)len, prot, flags, m->fd, (off_t)off);
    if (p == MAP_FAILED) {
        set_last_error(ERR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
#endif
    std::lock_guard<std::mutex> lk(g_files);
    g_views[(uintptr_t)p] = View{(uint32_t)len, m};
    return p;
}

uint32_t W32_STDCALL w32_UnmapViewOfFile(const void* view) {
    std::shared_ptr<MapObj> keep;          // (released after the lock: the mapping may end here)
    std::lock_guard<std::mutex> lk(g_files);
    auto it = g_views.upper_bound((uintptr_t)view);
    if (it == g_views.begin()) return fail(ERR_INVALID_ADDRESS);
    --it;
    if ((uintptr_t)view >= it->first + it->second.size) return fail(ERR_INVALID_ADDRESS);
#ifdef _WIN32
    if (!UnmapViewOfFile((void*)it->first)) return fail(GetLastError());
#else
    munmap((void*)it->first, it->second.size);
#endif
    keep = std::move(it->second.map);
    g_views.erase(it);
    return 1;
}

// =====================================================================================================================
// not available
// =====================================================================================================================
w32_HANDLE W32_STDCALL w32_CreateMailslotA(const char*, uint32_t, uint32_t, void*) {
    return fail_handle(ERR_NOT_SUPPORTED);
}

uint32_t W32_STDCALL w32_DeviceIoControl(w32_HANDLE h, uint32_t, void*, uint32_t, void*, uint32_t, uint32_t*,
                                         Overlapped*) {
    if (!get(h)) return fail(ERR_INVALID_HANDLE);
    return fail(ERR_INVALID_FUNCTION);
}

// =====================================================================================================================
// the console: Windows' own behind the stand-ins; a terminal on Linux
// =====================================================================================================================
uint32_t W32_STDCALL w32_AllocConsole() {
#ifdef _WIN32
    if (!AllocConsole()) return fail(GetLastError());
    std::lock_guard<std::mutex> lk(g_std_mu);
    std_init_locked(true);                         // the new console's handles are the standard ones now
    return 1;
#else
    if (isatty(0) || isatty(1)) return fail(ERR_ACCESS_DENIED);   // already a console (the terminal)
    std::lock_guard<std::mutex> lk(g_std_mu);
    std_init_locked(false);
    return 1;
#endif
}

uint32_t W32_STDCALL w32_FreeConsole() {
#ifdef _WIN32
    return FreeConsole() ? 1 : fail(GetLastError());
#else
    return 1;
#endif
}

uint32_t W32_STDCALL w32_ReadConsoleA(w32_HANDLE h, void* buf, uint32_t n, uint32_t* read, void* control) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadConsoleA(os_handle(*f), buf, n, &got, (PCONSOLE_READCONSOLE_CONTROL)control)) return fail(GetLastError());
    if (read) *read = got;
    return 1;
#else
    (void)control;
    if (!is_console(*f)) return fail(ERR_INVALID_HANDLE);
    // a line at a time, ending "\r\n" as Windows' line input does; what doesn't fit waits for the next call
    std::lock_guard<std::mutex> lk(g_console_mu);
    if (g_console_pending.empty()) {
        std::string line;
        char c;
        for (;;) {
            const ssize_t r = ::read(f->fd, &c, 1);
            if (r <= 0) break;
            if (c == '\n') {
                line += "\r\n";
                break;
            }
            line += c;
        }
        g_console_pending = line;
    }
    const uint32_t k = (uint32_t)std::min<size_t>(n, g_console_pending.size());
    memcpy(buf, g_console_pending.data(), k);
    g_console_pending.erase(0, k);
    if (read) *read = k;
    return 1;
#endif
}

uint32_t W32_STDCALL w32_WriteConsoleA(w32_HANDLE h, const void* buf, uint32_t n, uint32_t* written, void*) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    DWORD put = 0;
    if (!WriteConsoleA(os_handle(*f), buf, n, &put, 0)) return fail(GetLastError());
    if (written) *written = put;
    return 1;
#else
    if (!is_console(*f)) return fail(ERR_INVALID_HANDLE);
    uint32_t put = 0;
    while (put < n) {
        const ssize_t r = ::write(f->fd, (const char*)buf + put, n - put);
        if (r <= 0) break;
        put += (uint32_t)r;
    }
    if (written) *written = put;
    return 1;
#endif
}

uint32_t W32_STDCALL w32_SetConsoleMode(w32_HANDLE h, uint32_t mode) {
    std::shared_ptr<FileObj> f = file_of(h);
    if (!f) return fail(ERR_INVALID_HANDLE);
#ifdef _WIN32
    return SetConsoleMode(os_handle(*f), mode) ? 1 : fail(GetLastError());
#else
    if (!is_console(*f)) return fail(ERR_INVALID_HANDLE);
    if (f->fd != 0) return 1;                      // output modes: the terminal does them
    termios t;
    if (tcgetattr(0, &t) != 0) return fail(ERR_INVALID_HANDLE);
    // ENABLE_PROCESSED_INPUT 1 (Ctrl+C is a signal), ENABLE_LINE_INPUT 2, ENABLE_ECHO_INPUT 4
    if (mode & 1) t.c_lflag |= ISIG; else t.c_lflag &= ~ISIG;
    if (mode & 2) t.c_lflag |= ICANON; else t.c_lflag &= ~ICANON;
    if (mode & 4) t.c_lflag |= ECHO; else t.c_lflag &= ~ECHO;
    return tcsetattr(0, TCSANOW, &t) == 0 ? 1 : fail(ERR_INVALID_HANDLE);
#endif
}

uint32_t W32_STDCALL w32_SetConsoleTitleA(const char* title) {
#ifdef _WIN32
    return SetConsoleTitleA(title) ? 1 : fail(GetLastError());
#else
    if (isatty(1)) {
        const std::string s = std::string("\033]0;") + (title ? title : "") + "\007";
        ssize_t r = ::write(1, s.data(), s.size());
        (void)r;
    }
    return 1;
#endif
}

// =====================================================================================================================
// the last error
// =====================================================================================================================
uint32_t W32_STDCALL w32_GetLastError() { return last_error(); }
void W32_STDCALL w32_SetLastError(uint32_t e) { set_last_error(e); }
