// w32_path.cpp -- the stand-ins' path layer (see w32_path.h): Windows' full-path rules, the name order and wildcard
// match of a directory listing, 8.3 names, and the OS calls underneath the file stand-ins.
//
// Every rule here was measured against Windows 11 (test/w32_file_test.cpp runs them side by side with the real API):
//   * full paths: "." and ".." components; a component followed by a backslash loses ONE trailing dot ("a.\b" ->
//     "a\b", "a..\b" stays); the last component loses all trailing dots and spaces; repeated separators collapse;
//     ".." never climbs above the root (C:\, \\server\share, \\.\); a drive-relative path on another drive is taken
//     from that drive's root (the "=D:" variables are no longer read); "NUL" (with trailing dots, spaces or a colon) is
//     the device \\.\NUL in any folder, CON / PRN / AUX / COMn / LPTn / CONIN$ / CONOUT$ only as a bare name;
//     a path or a result of MAX_PATH (260) characters or more is ERROR_FILENAME_EXCED_RANGE.
//   * listings: NTFS keeps a folder's names sorted by their upper-cased UTF-16 form, ordinal; a name that isn't a valid
//     8.3 name has a second entry, its short name, in the same order. A search returns each file once: at its long
//     name if that matches the pattern, else at its short name if that does -- so "*." lists ".hidden" at "HIDDEN~1".
//   * patterns: Win32's '*' before a '.' is DOS_STAR (up to the last dot), '?' is DOS_QM (one character, or none at a
//     dot or the end), a '.' before '*' / '?' / the end is DOS_DOT (a dot, or nothing at the end); "*.*" is '*'.
//     "." and ".." are listed when the pattern has nothing but wildcards and dots.
#define _FILE_OFFSET_BITS 64
#include "w32_path.h"
#include "w32_handle.h"
#include <string.h>
#include <errno.h>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#endif

namespace w32 {
namespace path {

namespace {

inline bool is_sep(char c) { return c == '\\' || c == '/'; }
inline char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

bool ieq(const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (up(a[i]) != up(b[i])) return false;
    return true;
}

// a parsed full path: its root ("C:\", "\\srv\share", "\\srv\", "\\.\", ...) and its components
struct Parsed {
    std::string root;
    Kind kind;
    std::vector<std::string> comps;
    bool trailing;            // ends with a separator
};

// a component followed by a separator loses one trailing dot ("a." -> "a", "a.." and "..." stay)
void strip_inner_dot(std::string& s) {
    size_t n = s.size();
    if (n >= 2 && s[n - 1] == '.' && s[n - 2] != '.') s.erase(n - 1);
}

// the components of `rest` applied to p ("." dropped, ".." pops, never above the root)
void apply(Parsed& p, const char* rest) {
    std::vector<std::string> toks;
    const char* s = rest;
    bool trailing = false;
    while (*s) {
        while (is_sep(*s)) s++;
        if (!*s) {
            trailing = true;
            break;
        }
        const char* e = s;
        while (*e && !is_sep(*e)) e++;
        toks.push_back(std::string(s, e - s));
        s = e;
        if (*s && !s[1]) trailing = true;      // (a single separator at the end)
    }
    for (size_t i = 0; i < toks.size(); i++) {
        std::string& t = toks[i];
        const bool last = i + 1 == toks.size() && !trailing;
        if (t == ".") continue;
        if (t == "..") {
            if (!p.comps.empty()) p.comps.pop_back();
            continue;
        }
        if (!last) strip_inner_dot(t);
        p.comps.push_back(t);
    }
    p.trailing = trailing;
    // the end of the path loses its trailing spaces and dots (never into the root)
    if (!trailing && !p.comps.empty()) {
        std::string& l = p.comps.back();
        size_t n = l.size();
        while (n && (l[n - 1] == ' ' || l[n - 1] == '.')) n--;
        l.erase(n);
    }
}

std::string join(const Parsed& p, size_t* part) {
    std::string out = p.root;
    for (size_t i = 0; i < p.comps.size(); i++) {
        if (out.empty() || !is_sep(out.back())) out += '\\';
        if (i + 1 == p.comps.size() && part) *part = p.comps[i].empty() || p.trailing ? std::string::npos : out.size();
        out += p.comps[i];
    }
    if (p.trailing && (out.empty() || out.back() != '\\')) out += '\\';
    return out;
}

// an absolute path's root (and where its components start); false for a relative or drive-relative path
bool parse_root(const char* in, Parsed& p, const char** rest) {
    if (is_sep(in[0]) && is_sep(in[1])) {
        if ((in[2] == '.' || in[2] == '?') && (is_sep(in[3]) || !in[3])) {    // \\.\x, \\?\x, \\., \\?
            p.root = std::string("\\\\") + in[2] + "\\";
            p.kind = DEVICE;
            *rest = in[3] ? in + 4 : in + 3;
            return true;
        }
        const char* s = in + 2;                  // \\server\share: ".." stops at the share
        const char* e = s;
        while (*e && !is_sep(*e)) e++;
        p.root = "\\\\" + std::string(s, e - s);
        p.kind = UNC;
        if (*e) {
            p.root += '\\';
            s = e + 1;
            e = s;
            while (*e && !is_sep(*e)) e++;
            p.root.append(s, e - s);
        }
        *rest = e;
        return true;
    }
    if (in[0] && in[1] == ':' && is_sep(in[2])) {
        p.root = std::string(in, 2) + "\\";
        p.kind = DRIVE;
        *rest = in + 3;
        return true;
    }
    return false;
}

const char* const k_devices[] = {"CON", "PRN", "AUX", "CONIN$", "CONOUT$", "COM1", "COM2", "COM3", "COM4", "COM5",
                                 "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7",
                                 "LPT8", "LPT9"};

// Windows 11's DOS devices: NUL in any folder of a drive path, the others only as a bare relative name
bool device_name(const char* in, std::string& out) {
    if (is_sep(in[0]) && is_sep(in[1])) return false;
    const char* last = in;
    bool bare = !(in[0] && in[1] == ':');
    for (const char* s = in; *s; s++)
        if (is_sep(*s)) {
            last = s + 1;
            bare = false;
        }
    if (last == in && in[0] && in[1] == ':') last = in + 2;
    std::string n(last);
    size_t k = n.size();
    while (k && (n[k - 1] == ' ' || n[k - 1] == '.')) k--;
    if (k && n[k - 1] == ':') k--;
    while (k && (n[k - 1] == ' ' || n[k - 1] == '.')) k--;
    n.erase(k);
    bool dev = ieq(n, "NUL");
    if (!dev && bare)
        for (const char* d : k_devices) dev = dev || ieq(n, d);
    if (!dev) return false;
    out = "\\\\.\\" + n;
    return true;
}

#ifndef _WIN32
// Linux: the Windows-form current directory and the install folder (drive C:'s root)
std::mutex g_mu;
std::string g_cwd = "C:\\";
std::string g_root = ".";
#endif

}  // namespace

std::string cwd() {
#ifdef _WIN32
    char buf[MAX_PATH_ + 1];
    if (!_getcwd(buf, sizeof buf)) return "C:\\";
    return buf;
#else
    std::lock_guard<std::mutex> lk(g_mu);
    return g_cwd;
#endif
}

bool full(const char* in, std::string& out, size_t* part, uint32_t* err) {
    if (part) *part = std::string::npos;
    if (!in) {
        *err = ERR_INVALID_PARAMETER;
        return false;
    }
    const size_t n = strlen(in);
    size_t blanks = 0;
    while (in[blanks] == ' ') blanks++;
    if (n == 0 || blanks == n) {
        *err = ERR_INVALID_NAME;
        return false;
    }
    if (n >= MAX_PATH_) {
        *err = ERR_FILENAME_EXCED_RANGE;
        return false;
    }
    if (device_name(in, out)) return true;
    Parsed p;
    const char* rest;
    if (!parse_root(in, p, &rest)) {
        // relative to the current directory: "x", "\x" (its root), "X:x" (the current directory on its drive, else
        // that drive's root)
        const std::string base = cwd();
        Parsed b;
        const char* brest;
        if (!parse_root(base.c_str(), b, &brest)) {
            b.root = "C:\\";
            b.kind = DRIVE;
            brest = "";
        }
        // (a UNC root taken from the current directory keeps its backslash: "\\srv\share\")
        if (b.kind == UNC && b.root.back() != '\\') b.root += '\\';
        if (is_sep(in[0])) {
            p = b;
            p.comps.clear();
            rest = in + 1;
        } else if (in[0] && in[1] == ':') {
            if (b.kind == DRIVE && up(in[0]) == up(b.root[0])) {
                p = b;
                apply(p, brest);
            } else {
                p.root = std::string(1, up(in[0])) + ":\\";
                p.kind = DRIVE;
#ifdef _WIN32
                // another drive's current directory: its "=X:" variable, when that names a folder that exists (the
                // variable is used as written by Windows: one ending in a backslash gets a second -- not kept here)
                char var[4] = {'=', up(in[0]), ':', 0}, val[MAX_PATH_];
                const DWORD k = GetEnvironmentVariableA(var, val, sizeof val);
                const DWORD a = k && k < sizeof val ? GetFileAttributesA(val) : INVALID_FILE_ATTRIBUTES;
                Parsed v;
                const char* vrest;
                if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && parse_root(val, v, &vrest)) {
                    p = v;
                    apply(p, vrest);
                }
#endif
            }
            rest = in + 2;
        } else {
            p = b;
            apply(p, brest);
            rest = in;
        }
    }
    p.trailing = false;
    apply(p, rest);
    out = join(p, part);
    if (out.size() >= MAX_PATH_) {
        *err = ERR_FILENAME_EXCED_RANGE;
        if (part) *part = std::string::npos;
        return false;
    }
    return true;
}

std::string key(const std::string& full) {
    std::string k(full);
    for (char& c : k) c = c == '/' ? '\\' : up(c);
    return k;
}

// ---- names ------------------------------------------------------------------------------------------------------------
namespace {
// a byte of the ANSI code page (1252) as UTF-16
uint16_t uni_1252(uint8_t c) {
    static const uint16_t hi[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                    0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    return c >= 0x80 && c < 0xA0 ? hi[c - 0x80] : c;
}
// ... upper-cased as NTFS' upcase table has it
uint16_t upcase_1252(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32);
    if (c == 0x9A || c == 0x9C || c == 0x9E) return (uint16_t)(uni_1252(c) - 1);   // s-caron, oe, z-caron
    if (c < 0xA0) return uni_1252(c);
    if (c == 0xB5) return 0x039C;              // micro sign -> Greek capital mu
    if (c == 0xFF) return 0x0178;
    if (c >= 0xE0 && c != 0xF7) return (uint16_t)(c - 0x20);
    return c;
}
}  // namespace

int compare_names(const char* a, const char* b) {
    for (;; a++, b++) {
        const uint16_t x = upcase_1252((uint8_t)*a), y = upcase_1252((uint8_t)*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}

namespace {
const char DOS_STAR = '<', DOS_QM = '>', DOS_DOT = '"';

// FsRtlIsNameInExpression's DOS rules, case-insensitive (expr already converted)
bool match_at(const char* e, const char* n, const char* lastdot) {
    for (;;) {
        const char c = *e;
        if (!c) return !*n;
        if (c == '*') {
            for (const char* t = n;; t++) {
                if (match_at(e + 1, t, lastdot)) return true;
                if (!*t) return false;
            }
        }
        if (c == DOS_STAR) {
            // zero or more characters, not past the name's last dot (which it may take); after that dot, any
            const char* limit = lastdot && n <= lastdot ? lastdot + 1 : nullptr;
            for (const char* t = n;; t++) {
                if (match_at(e + 1, t, lastdot)) return true;
                if (!*t || (limit && t >= limit)) return false;
            }
        }
        if (c == DOS_QM) {
            if (!*n || *n == '.') {               // nothing at a dot or the end
                e++;
                continue;
            }
            e++;
            n++;
            continue;
        }
        if (c == DOS_DOT) {
            if (*n == '.') {
                e++;
                n++;
                continue;
            }
            if (!*n) {
                e++;
                continue;
            }
            return false;
        }
        if (c == '?') {
            if (!*n) return false;
            e++;
            n++;
            continue;
        }
        if (upcase_1252((uint8_t)c) != upcase_1252((uint8_t)*n)) return false;
        e++;
        n++;
    }
}

std::string convert_pattern(const char* p) {
    std::string s(p);
    if (s == "*.*") return "*";
    for (size_t i = 0; i < s.size(); i++) {
        const char nx = i + 1 < s.size() ? s[i + 1] : 0;
        if (s[i] == '?') s[i] = DOS_QM;
        else if (s[i] == '*' && nx == '.') s[i] = DOS_STAR;
        else if (s[i] == '.' && (nx == '?' || nx == '*' || nx == 0)) s[i] = DOS_DOT;
    }
    return s;
}
}  // namespace

bool match(const char* pattern, const char* name) {
    const std::string e = convert_pattern(pattern);
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        // the folder's own entries: listed for a pattern of wildcards and dots alone
        for (char c : e)
            if (c != '*' && c != DOS_STAR && c != DOS_QM && c != DOS_DOT && c != '.') return false;
        return true;
    }
    return match_at(e.c_str(), name, strrchr(name, '.'));
}

namespace {
// the characters an 8.3 name may have as they are (others are replaced or dropped)
bool short_char(char c) {
    const uint8_t u = (uint8_t)c;
    if (u >= 0x80) return false;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    return strchr("!#$%&'()-@^_`{}~", c) != 0;
}
}  // namespace

bool is_short_name(const char* name) {
    const char* dot = strchr(name, '.');
    const size_t n = strlen(name);
    if (!n || name[0] == '.') return false;
    const size_t base = dot ? (size_t)(dot - name) : n;
    if (base == 0 || base > 8) return false;
    if (dot && (strchr(dot + 1, '.') || n - base - 1 > 3 || n - base - 1 == 0)) return false;
    for (size_t i = 0; i < n; i++)
        if (name + i != dot && !short_char(name[i])) return false;
    return true;
}

std::string make_short_name(const char* name, int idx) {
    // RtlGenerate8dot3Name: the base (before the last dot; dots and spaces dropped, "+,;=[]" as '_', other
    // characters a short name can't have dropped) cut to 6 characters, "~n", the extension's first three characters.
    // A base of two characters or fewer takes a checksum of the long name as four hex digits after them.
    const char* dot = strrchr(name, '.');
    if (dot == name) dot = 0;                  // ".hidden": no extension; the name is the base
    std::string base, ext;
    const char* end = dot ? dot : name + strlen(name);
    for (const char* s = name; s < end; s++) {
        const char c = *s;
        if (c == '.' || c == ' ') continue;
        if (strchr("+,;=[]", c)) base += '_';
        else if (short_char(c)) base += up(c);
    }
    if (dot)
        for (const char* s = dot + 1; *s && ext.size() < 3; s++) {
            const char c = *s;
            if (c == '.' || c == ' ') continue;
            if (strchr("+,;=[]", c)) ext += '_';
            else if (short_char(c)) ext += up(c);
        }
    char num[16];
    snprintf(num, sizeof num, "~%d", idx);
    if (base.size() <= 2) {
        // the checksum of Windows 7 and later over the long name's UTF-16 units: h = 37h + c; h *= 314159269; |h|
        // mod 1000000007; its low four hex digits in reverse order. (It gives Windows' digits for some names, not all:
        // Windows was seen to make others for the same name. Only the digits differ -- the base, "~n" and the
        // extension, all a wildcard pattern can match on, are Windows'.)
        int32_t h = 0;
        for (const char* s = name; *s; s++) h = (int32_t)(37u * (uint32_t)h + uni_1252((uint8_t)*s));
        h = (int32_t)(314159269u * (uint32_t)h);
        int64_t a = h < 0 ? -(int64_t)h : (int64_t)h;
        const uint32_t m = (uint32_t)(a % 1000000007);
        const uint32_t r = ((m & 0xF) << 12) | ((m & 0xF0) << 4) | ((m >> 4) & 0xF0) | ((m >> 12) & 0xF);
        char hx[8];
        snprintf(hx, sizeof hx, "%04X", r);
        base += hx;
    }
    const size_t keep = 8 - strlen(num);
    if (base.size() > keep) base.erase(keep);
    std::string out = base + num;
    if (!ext.empty()) out += "." + ext;
    return out;
}

// ---- current directory, drives, the Linux root -----------------------------------------------------------------------
bool set_cwd(const char* in, uint32_t* err) {
    std::string f;
    if (!full(in, f, 0, err)) return false;
    Resolved r;
    if (!resolve(f.c_str(), r, err)) return false;
    os::Info info;
    if (!os::stat(r.host, info, err)) return false;
    if (!(info.attrs & ATTR_DIRECTORY)) {
        *err = ERR_DIRECTORY;
        return false;
    }
#ifdef _WIN32
    return os::chdir(r.host, err);
#else
    // the Windows spelling is kept (without a trailing backslash but on a root); the host follows
    if (f.size() > 3 && f.back() == '\\') f.pop_back();
    if (!os::chdir(r.host, err)) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_cwd = f;
    return true;
#endif
}

uint32_t drive_type(const char* root) {
    // a root: "X:\", "X:" or the current drive (null); anything else has no root directory
    std::string r;
    if (!root) {
        const std::string c = cwd();
        r = c.size() >= 2 && c[1] == ':' ? c.substr(0, 2) + "\\" : c;
    } else {
        r = root;
        if (r.size() == 2 && r[1] == ':') r += '\\';
    }
#ifdef _WIN32
    return GetDriveTypeA(r.c_str());
#else
    if (r.size() == 3 && r[1] == ':' && is_sep(r[2]) && up(r[0]) == 'C') return 3;   // DRIVE_FIXED: the install
    return 1;                                                                          // DRIVE_NO_ROOT_DIR
#endif
}

void set_root(const char* host_dir) {
#ifdef _WIN32
    (void)host_dir;
#else
    std::lock_guard<std::mutex> lk(g_mu);
    g_root = host_dir;
    while (g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();
    g_cwd = "C:\\";
#endif
}

// ---- Windows path -> host path --------------------------------------------------------------------------------------
#ifndef _WIN32
namespace {
// the entry of host folder `dir` called `name`, any case (exact first; then a case-insensitive match; then an 8.3 name)
bool find_entry(const std::string& dir, const std::string& name, std::string& found) {
    struct stat st;
    if (lstat((dir + "/" + name).c_str(), &st) == 0) {
        found = name;
        return true;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    bool ok = false;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (compare_names(e->d_name, name.c_str()) == 0) {
            found = e->d_name;
            ok = true;
            break;
        }
        names.push_back(e->d_name);
    }
    closedir(d);
    if (ok || name.find('~') == std::string::npos) return ok;
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return compare_names(a.c_str(), b.c_str()) < 0;
    });
    std::vector<std::string> made;
    for (const std::string& n : names) {
        if (is_short_name(n.c_str())) continue;
        for (int i = 1;; i++) {
            const std::string s = make_short_name(n.c_str(), i);
            if (std::find(made.begin(), made.end(), s) != made.end()) continue;
            made.push_back(s);
            if (compare_names(s.c_str(), name.c_str()) == 0) {
                found = n;
                return true;
            }
            break;
        }
    }
    return false;
}
}  // namespace
#endif

bool resolve(const char* in, Resolved& r, uint32_t* err) {
    if (!full(in, r.full, 0, err)) return false;
    const std::string& f = r.full;
    r.kind = f.size() >= 4 && f[0] == '\\' && f[1] == '\\' && (f[2] == '.' || f[2] == '?') && f[3] == '\\' ? DEVICE
             : f.size() >= 2 && f[0] == '\\' && f[1] == '\\'                                             ? UNC
                                                                                                          : DRIVE;
    r.parent_found = true;
#ifdef _WIN32
    r.host = f;                                 // Windows resolves it
    return true;
#else
    if (r.kind == DEVICE) {
        *err = ERR_FILE_NOT_FOUND;              // no such device
        return false;
    }
    if (r.kind == UNC) {
        *err = ERR_BAD_NETPATH;
        return false;
    }
    if (up(f[0]) != 'C') {
        *err = ERR_PATH_NOT_FOUND;              // no such drive
        return false;
    }
    std::string host;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        host = g_root;
    }
    std::vector<std::string> comps;
    for (size_t s = 3; s < f.size();) {
        size_t e = f.find('\\', s);
        if (e == std::string::npos) e = f.size();
        if (e > s) comps.push_back(f.substr(s, e - s));
        s = e + 1;
    }
    bool missing = false;
    for (size_t i = 0; i < comps.size(); i++) {
        std::string found;
        if (!missing && find_entry(host, comps[i], found)) {
            host += "/" + found;
            if (i + 1 < comps.size()) {
                struct stat st;
                if (stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) missing = true;
            }
        } else {
            host += "/" + comps[i];
            if (i + 1 < comps.size()) missing = true;
        }
    }
    r.parent_found = !missing;
    r.host = host;
    return true;
#endif
}

}  // namespace path

// ---- the OS calls ---------------------------------------------------------------------------------------------------
namespace os {

uint32_t error_from_errno(int e, bool missing_parent) {
    switch (e) {
    case 0: return ERR_SUCCESS;
    case ENOENT: return missing_parent ? ERR_PATH_NOT_FOUND : ERR_FILE_NOT_FOUND;
    case ENOTDIR: return ERR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EISDIR: return ERR_ACCESS_DENIED;
    case EEXIST: return ERR_FILE_EXISTS;
    case EMFILE: case ENFILE: return ERR_TOO_MANY_OPEN_FILES;
    case EBADF: return ERR_INVALID_HANDLE;
    case ENOMEM: return ERR_NOT_ENOUGH_MEMORY;
    case ENOSPC: return ERR_DISK_FULL;
    case EROFS: return ERR_WRITE_PROTECT;
    case ENAMETOOLONG: return ERR_FILENAME_EXCED_RANGE;
    case ENOTEMPTY: return ERR_DIR_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return ERR_SHARING_VIOLATION;
    case EINVAL: return ERR_INVALID_PARAMETER;
    default: return ERR_ACCESS_DENIED;
    }
}

#ifdef _WIN32
namespace {
// the Win32 error behind a failed C runtime call (its _doserrno), else from errno
uint32_t crt_error() {
    const unsigned long d = _doserrno;
    return d ? (uint32_t)d : error_from_errno(errno, false);
}
#define CRT_CALL(expr) (_doserrno = 0, errno = 0, (expr))
FileTime ft(const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }
}  // namespace

bool stat(const std::string& host, Info& info, uint32_t* err) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(host.c_str(), GetFileExInfoStandard, &d)) {
        *err = GetLastError();
        return false;
    }
    info.attrs = d.dwFileAttributes;
    info.size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    info.created = ft(d.ftCreationTime);
    info.accessed = ft(d.ftLastAccessTime);
    info.written = ft(d.ftLastWriteTime);
    info.reserved0 = 0;
    return true;
}

bool list(const std::string& dir, std::vector<Info>& out, uint32_t* err) {
    std::string pat = dir;
    if (pat.empty() || pat.back() != '\\') pat += '\\';
    pat += '*';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        *err = e == ERROR_FILE_NOT_FOUND ? ERROR_NO_MORE_FILES : e;   // (an empty root: nothing to list)
        if (e == ERROR_FILE_NOT_FOUND) return true;
        return false;
    }
    do {
        Info i;
        i.name = fd.cFileName;
        i.short_name = fd.cAlternateFileName;
        i.attrs = fd.dwFileAttributes;
        i.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        i.created = ft(fd.ftCreationTime);
        i.accessed = ft(fd.ftLastAccessTime);
        i.written = ft(fd.ftLastWriteTime);
        i.reserved0 = fd.dwReserved0;
        out.push_back(i);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return true;
}

int open(const std::string& host, int flags, uint32_t attrs, uint32_t* err) {
    DWORD access = 0;
    if (flags & O_READ) access |= GENERIC_READ;
    if (flags & O_WRITE) access |= GENERIC_WRITE;
    const DWORD disp = (flags & O_CREATE) ? ((flags & O_EXCLUSIVE) ? CREATE_NEW : (flags & O_TRUNCATE) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                          : (flags & O_TRUNCATE) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    const DWORD fa = attrs & ~(ATTR_DIRECTORY | ATTR_NORMAL) ? attrs & ~(ATTR_DIRECTORY | ATTR_NORMAL) : FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileA(host.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, disp, fa, 0);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return -1;
    }
    const int fd = _open_osfhandle((intptr_t)h, _O_BINARY | ((flags & O_WRITE) ? 0 : _O_RDONLY));
    if (fd < 0) {
        CloseHandle(h);
        *err = ERROR_TOO_MANY_OPEN_FILES;
    }
    return fd;
}
bool close(int fd) { return _close(fd) == 0; }
int64_t read(int fd, void* buf, uint32_t n, uint32_t* err) {
    const int r = CRT_CALL(_read(fd, buf, n));
    if (r < 0) *err = crt_error();
    return r;
}
int64_t write(int fd, const void* buf, uint32_t n, uint32_t* err) {
    const int r = CRT_CALL(_write(fd, buf, n));
    if (r < 0) *err = crt_error();
    return r;
}
int64_t seek(int fd, int64_t pos, int whence, uint32_t* err) {
    const int64_t r = CRT_CALL(_lseeki64(fd, pos, whence));
    if (r < 0) *err = crt_error();
    return r;
}
int64_t size(int fd, uint32_t* err) {
    const int64_t r = CRT_CALL(_filelengthi64(fd));
    if (r < 0) *err = crt_error();
    return r;
}
bool truncate(int fd, int64_t n, uint32_t* err) {
    if (CRT_CALL(_chsize_s(fd, n)) != 0) {
        *err = crt_error();
        return false;
    }
    return true;
}
bool flush(int fd, uint32_t* err) {
    if (CRT_CALL(_commit(fd)) != 0) {
        *err = crt_error();
        return false;
    }
    return true;
}
bool mkdir(const std::string& host, uint32_t* err) {
    if (CRT_CALL(_mkdir(host.c_str())) != 0) {
        *err = crt_error();
        return false;
    }
    return true;
}
bool unlink(const std::string& host, uint32_t* err) {
    if (CRT_CALL(_unlink(host.c_str())) != 0) {
        *err = crt_error();
        return false;
    }
    return true;
}
bool set_attributes(const std::string& host, uint32_t attrs, uint32_t* err) {
    if (!SetFileAttributesA(host.c_str(), attrs)) {
        *err = GetLastError();
        return false;
    }
    return true;
}
bool set_written(const std::string& host, FileTime written, uint32_t* err) {
    HANDLE h = CreateFileA(host.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    FILETIME w;
    w.dwLowDateTime = (DWORD)written;
    w.dwHighDateTime = (DWORD)(written >> 32);
    const BOOL ok = SetFileTime(h, 0, 0, &w);
    if (!ok) *err = GetLastError();
    CloseHandle(h);
    return ok != 0;
}
bool chdir(const std::string& host, uint32_t* err) {
    if (CRT_CALL(_chdir(host.c_str())) != 0) {
        *err = crt_error();
        return false;
    }
    return true;
}

#else  // POSIX (R2b): the same calls on Linux

namespace {
FileTime ft(const struct timespec& t) {
    return ((uint64_t)(int64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100;
}
bool missing_parent(const std::string& host) {
    const size_t s = host.rfind('/');
    if (s == std::string::npos) return false;
    struct stat st;
    return ::stat(s ? host.substr(0, s).c_str() : "/", &st) != 0 || !S_ISDIR(st.st_mode);
}
uint32_t err_of(const std::string& host) {
    if (errno == ENOTDIR && !host.empty() && host.back() == '/') return ERR_DIRECTORY;   // ("file\": not a folder)
    return error_from_errno(errno, errno == ENOENT && missing_parent(host));
}
void fill(const struct stat& st, Info& info) {
    // attributes: a folder, else an archive (what Windows gives a file it made or copied); read-only without 'w'.
    // Hidden / system / not-archived aren't kept on Linux.
    info.attrs = S_ISDIR(st.st_mode) ? ATTR_DIRECTORY : ATTR_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) info.attrs |= ATTR_READONLY;
    info.size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    info.written = ft(st.st_mtim);
    info.accessed = ft(st.st_atim);
    info.created = info.written < ft(st.st_ctim) ? info.written : ft(st.st_ctim);   // (no birth time in stat)
    info.reserved0 = 0;
}
}  // namespace

bool stat(const std::string& host, Info& info, uint32_t* err) {
    struct stat st;
    if (::stat(host.c_str(), &st) != 0) {
        *err = err_of(host);
        return false;
    }
    fill(st, info);
    return true;
}

bool list(const std::string& dir, std::vector<Info>& out, uint32_t* err) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        *err = err_of(dir);
        return false;
    }
    std::vector<std::string> longs;
    while (dirent* e = readdir(d)) {
        Info i;
        struct stat st;
        if (::stat((dir + "/" + e->d_name).c_str(), &st) != 0) continue;
        i.name = e->d_name;
        fill(st, i);
        out.push_back(i);
    }
    closedir(d);
    // 8.3 names as Windows would make them: ~1, ~2, ... in name order (Windows numbers them in creation order)
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
        return path::compare_names(a.name.c_str(), b.name.c_str()) < 0;
    });
    std::vector<std::string> made;
    for (Info& i : out) {
        if (i.name == "." || i.name == ".." || path::is_short_name(i.name.c_str())) continue;
        for (int k = 1; k < 1000; k++) {
            const std::string s = path::make_short_name(i.name.c_str(), k);
            if (std::find(made.begin(), made.end(), s) != made.end()) continue;
            made.push_back(s);
            i.short_name = s;
            break;
        }
    }
    return true;
}

int open(const std::string& host, int flags, uint32_t attrs, uint32_t* err) {
    int o = O_CLOEXEC;
    o |= (flags & O_READ) && (flags & O_WRITE) ? O_RDWR : (flags & O_WRITE) ? O_WRONLY : O_RDONLY;
    if (flags & O_CREATE) o |= O_CREAT;
    if (flags & O_TRUNCATE) o |= O_TRUNC;
    if (flags & O_EXCLUSIVE) o |= O_EXCL;
    const int fd = ::open(host.c_str(), o, (attrs & ATTR_READONLY) ? 0444 : 0666);
    if (fd < 0) *err = err_of(host);
    return fd;
}
bool close(int fd) { return ::close(fd) == 0; }
int64_t read(int fd, void* buf, uint32_t n, uint32_t* err) {
    const ssize_t r = ::read(fd, buf, n);
    if (r < 0) *err = error_from_errno(errno, false);
    return r;
}
int64_t write(int fd, const void* buf, uint32_t n, uint32_t* err) {
    const ssize_t r = ::write(fd, buf, n);
    if (r < 0) *err = error_from_errno(errno, false);
    return r;
}
int64_t seek(int fd, int64_t pos, int whence, uint32_t* err) {
    const off_t r = ::lseek(fd, (off_t)pos, whence);
    if (r < 0) *err = error_from_errno(errno, false);
    return r;
}
int64_t size(int fd, uint32_t* err) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        *err = error_from_errno(errno, false);
        return -1;
    }
    return st.st_size;
}
bool truncate(int fd, int64_t n, uint32_t* err) {
    if (ftruncate(fd, (off_t)n) != 0) {
        *err = error_from_errno(errno, false);
        return false;
    }
    return true;
}
bool flush(int fd, uint32_t* err) {
    if (fsync(fd) != 0) {
        *err = error_from_errno(errno, false);
        return false;
    }
    return true;
}
bool mkdir(const std::string& host, uint32_t* err) {
    if (::mkdir(host.c_str(), 0777) != 0) {
        *err = errno == EEXIST ? ERR_ALREADY_EXISTS : err_of(host);
        return false;
    }
    return true;
}
bool unlink(const std::string& host, uint32_t* err) {
    if (::unlink(host.c_str()) != 0) {
        *err = err_of(host);
        return false;
    }
    return true;
}
bool set_attributes(const std::string& host, uint32_t attrs, uint32_t* err) {
    struct stat st;
    if (::stat(host.c_str(), &st) != 0) {
        *err = err_of(host);
        return false;
    }
    mode_t m = st.st_mode & 07777;
    if (attrs & ATTR_READONLY) m &= ~(mode_t)0222;
    else m |= S_IWUSR;
    if (chmod(host.c_str(), m) != 0) {
        *err = err_of(host);
        return false;
    }
    return true;
}
bool set_written(const std::string& host, FileTime written, uint32_t* err) {
    struct timespec t[2];
    t[0].tv_sec = 0;
    t[0].tv_nsec = UTIME_OMIT;
    const uint64_t u = written - 116444736000000000ull;
    t[1].tv_sec = (time_t)(u / 10000000ull);
    t[1].tv_nsec = (long)(u % 10000000ull) * 100;
    if (utimensat(AT_FDCWD, host.c_str(), t, 0) != 0) {
        *err = err_of(host);
        return false;
    }
    return true;
}
bool chdir(const std::string& host, uint32_t* err) {
    if (::chdir(host.c_str()) != 0) {
        *err = err_of(host);
        return false;
    }
    return true;
}
#endif

}  // namespace os
}  // namespace w32
