// w32_file_test.cpp -- the file / folder / listing / mapping / path / console stand-ins (hook/w32_file.cpp,
// hook/w32_path.cpp) side by side with real Windows: each case calls the real KERNEL32 function and the stand-in with
// the same inputs and compares the return value, the out parameters and GetLastError (both preset to a marker, so "left
// alone" is seen too). Calls that change files run on twin folders (real\ and stub\) made the same; calls that only read
// run on the same folder. Prints each difference and a PASS / FAIL count; exit code 0 when everything agrees.
//
// Build (from the repo root; either compiler):
//   GCC:  C:\msys64\mingw32\bin\g++ -m32 -O2 -std=c++17 -Ihook test\w32_file_test.cpp hook\w32_file.cpp
//           hook\w32_path.cpp hook\w32_handle.cpp -o test\build\w32_file_test.exe -static
//   MSVC: (vcvarsall x86) cl /nologo /O2 /EHsc /std:c++17 /MT /Ihook test\w32_file_test.cpp hook\w32_file.cpp
//           hook\w32_path.cpp hook\w32_handle.cpp /Fetest\build\w32_file_test.exe /Fotest/build/
// Run: test\build\w32_file_test.exe [-v]   (works in %TEMP%\w32_file_test, removed at the end; -v lists every case)
// Result on Windows 11 (2026-10-05), both compilers: every case agrees.
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <functional>
#include "w32_file.h"
#include "w32_path.h"

static int g_pass, g_fail;
static bool g_verbose;
static std::string g_base;                    // %TEMP%\w32_file_test
static const uint32_t MARK = 0xBEEF0001u;

static std::string fmt(const char* f, ...) {
    char b[2048];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

// one call's observable result
struct R {
    uint32_t ret = 0;
    uint32_t err = 0;
    std::string out;                          // out parameters, buffers, file contents ... as text
    bool operator==(const R& o) const { return ret == o.ret && err == o.err && out == o.out; }
};
static std::string show(const R& r) {
    std::string e = r.err == MARK ? "untouched" : fmt("%u", r.err);
    return fmt("ret=%u (0x%x) err=%s out=[%s]", r.ret, r.ret, e.c_str(), r.out.c_str());
}
static void check(const std::string& what, const R& real, const R& stub) {
    if (real == stub) {
        g_pass++;
        if (g_verbose) printf("ok   %s: %s\n", what.c_str(), show(real).c_str());
        return;
    }
    g_fail++;
    if (real.ret == stub.ret && real.err == stub.err && real.out.size() > 300) {
        // a long output (a listing): the first line that differs
        size_t a = 0, b = 0;
        for (;;) {
            const size_t ea = real.out.find('\n', a), eb = stub.out.find('\n', b);
            const std::string la = real.out.substr(a, ea == std::string::npos ? std::string::npos : ea - a);
            const std::string lb = stub.out.substr(b, eb == std::string::npos ? std::string::npos : eb - b);
            if (la != lb || ea == std::string::npos || eb == std::string::npos) {
                printf("DIFF %s (line differs)\n     windows: %.400s\n     stand-in: %.400s\n", what.c_str(), la.c_str(),
                       lb.c_str());
                return;
            }
            a = ea + 1;
            b = eb + 1;
        }
    }
    printf("DIFF %s\n     windows: %s\n     stand-in: %s\n", what.c_str(), show(real).c_str(), show(stub).c_str());
}
static void expect(const std::string& what, bool ok, const std::string& note = "") {
    if (ok) {
        g_pass++;
        if (g_verbose) printf("ok   %s\n", what.c_str());
    } else {
        g_fail++;
        printf("FAIL %s %s\n", what.c_str(), note.c_str());
    }
}
static void mark() {
    SetLastError(MARK);
    w32::set_last_error(MARK);
}
static uint32_t real_err() { return GetLastError(); }
static uint32_t stub_err() { return w32::last_error(); }

// ---- the test folders ------------------------------------------------------------------------------------------------
static void rm_tree(const std::string& d) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = d + "\\" + fd.cFileName;
            SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(d.c_str());
}
static void put_file(const std::string& p, const std::string& data, DWORD attrs = 0) {
    HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    DWORD w;
    if (!data.empty()) WriteFile(h, data.data(), (DWORD)data.size(), &w, 0);
    CloseHandle(h);
    if (attrs) SetFileAttributesA(p.c_str(), attrs);
}
static std::string get_file(const std::string& p) {
    HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                           OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) return "<none>";
    std::string s;
    char b[4096];
    DWORD n;
    while (ReadFile(h, b, sizeof b, &n, 0) && n) s.append(b, n);
    CloseHandle(h);
    return s;
}
static std::string attrs_of(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a == INVALID_FILE_ATTRIBUTES ? "none" : fmt("%x", a);
}
// the twin folders of a case: base\real\<name> and base\stub\<name>, made by the same steps
struct Twin {
    std::string real, stub;
};
static Twin twin(const char* name, const std::function<void(const std::string&)>& make) {
    Twin t{g_base + "\\real\\" + name, g_base + "\\stub\\" + name};
    for (const std::string* d : {&t.real, &t.stub}) {
        rm_tree(*d);
        CreateDirectoryA(d->c_str(), 0);
        make(*d);
    }
    return t;
}
// a folder's state (names, attributes but archive, sizes, contents) for comparing twins
static std::string state(const std::string& d) {
    std::string s;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "<no folder>";
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        s += fmt("%s:%x:%u", fd.cFileName, fd.dwFileAttributes, fd.nFileSizeLow);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) s += "=" + get_file(d + "\\" + fd.cFileName);
        else s += "{" + state(d + "\\" + fd.cFileName) + "}";
        s += ";";
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return s;
}
static void same_state(const std::string& what, const Twin& t) {
    R a, b;
    a.out = state(t.real);
    b.out = state(t.stub);
    check(what + " (folder after)", a, b);
}

// ---- paths -----------------------------------------------------------------------------------------------------------
static const char* const k_paths[] = {
    "", ".", "..", "...", "a", "a.", "a..", "a. ", "a .", " a", "a\\", "a\\.", "a\\..", "a\\...\\b", "a.\\b", "a \\b",
    "a. \\b", "...\\b", "a\\.\\b", "a//b", "a\\\\b", "\\", "\\x", "/x", "//", "\\\\", "\\\\srv", "\\\\srv\\",
    "\\\\srv\\share", "\\\\srv\\share\\..\\..\\x", "\\\\.\\vexed.vxd", "\\\\.\\", "\\\\.", "\\\\?\\C:\\x\\..\\y",
    "\\\\.\\C:\\x\\..\\y", "C:", "C:.", "C:x", "D:.", "D:x", "c:\\", "c:\\..\\..", "c:/x/./y/", "c:\\x\\y.",
    "c:\\x\\y. . ", "c:\\x\\*.car", "con", "nul", "con.txt", "x\\nul", "aux", "COM1", "lpt1.x", "c:\\x:y", "c:\\x\\y ",
    "x \\", "x.\\", "\\\\srv\\share\\", "\\\\srv\\\\share", "C:x\\..\\..", "  ", " ", ".\\", "\\.", "\\..", "c:..",
    "c:..\\x", "a\\. \\b", "a\\.. \\b", "a\\ ..\\b", "nul.txt", "nul ", "nul.", "nul:", "COM9", "COM0", "LPT9", "CONIN$",
    "CONOUT$", "c:\\x\\con", "con\\x", "PRN", "x\\aux.", "x\\aux ", "\\\\.\\con", "//./x", "//?/x", "\\\\?/x", "\\/.\\x",
    "\\\\srv/share/x", "a\\..\\..\\..\\..\\..", "c:\\a\\b\\..", "c:\\a\\b\\.", "c:\\a\\.b", "c:\\a\\..b", "c:\\a.b.\\c",
    "c:\\a..\\c", "c:\\a. .\\c", "c:\\a .\\c", "c:\\a\\ ", "c:\\a\\. ", "c:\\a\\ .", "c:\\ ", "c:\\.", "c:\\. ",
    "\\\\srv\\share\\ ", "\\\\.\\ ", "\\\\.\\x. ", "c:\\a\\\\\\b", "\\\\\\x", "\\\\\\\\x", "c:\\x\\y\\..\\..\\..\\z",
    "\\\\?\\C:\\..\\..", "\\\\.\\x\\..\\..\\y", "c:\\.\\a", "c:x\\y", "1:\\x", "@:x", "C:\\x\\\\", "x\\\\",
    "\\\\srv\\share\\a\\..\\..", "\\\\?\\", "\\\\?", "x\\con", "c:\\x\\nul", "x\\aux", "aux.", "con ", "con.", "x\\nul.",
    "x\\nul ", "c:\\nul", "\\nul", "c:nul", "nul:x", "x\\nul.txt", "c:\\x\\nul.txt", "COM1:", "x\\COM1", "c:\\COM1",
    "x\\CONIN$", "c:\\CONOUT$", " nul", "nul..", "nul. .", "Nul", "a...\\c", "a. ..\\c", ".a.\\c", ". \\c", "a\\.\\",
    "a\\..\\", "   x", "a\\b\\.. ", "\\\\srv\\share\\..", "\\\\srv\\\\share\\..\\..", "c:\\a\\..\\.\\", "\\\\.\\x\\",
    "\\\\?\\x\\..", "\\\\.\\..", "c:\\a\\...", "c:\\a\\.. .", "c:/", "c:", "/", "\\\\.\\C:", "c:\\a:b\\c", "c:\\a<b",
    "c:\\a|b", "c:\\a\"b", "a \\b\\..", "a.b. \\c", "a\\b\\..\\..\\..\\c\\", "C:\\Users\\..\\Users\\.", "x/y\\z/",
    "data\\cars\\viper.car", "Config\\", "c:\\log.log", "Config-2\\x.ini", "\\\\.\\VEXED.VXD", "\\\\*\\mailslot\\mgi\\x"};

static R fullpath_real(const char* in, DWORD n, bool part) {
    R r;
    char buf[1024];
    memset(buf, 'X', sizeof buf);
    char* fp = (char*)1;
    mark();
    r.ret = GetFullPathNameA(in, n, n ? buf : 0, part ? &fp : 0);
    r.err = real_err();
    r.out = std::string(buf, buf + (n < 1024 ? n : 1024)).c_str();
    if (part) r.out += fp == (char*)1 ? "|untouched" : fp ? fmt("|part@%d", (int)(fp - buf)) : "|part=null";
    return r;
}
static R fullpath_stub(const char* in, DWORD n, bool part) {
    R r;
    char buf[1024];
    memset(buf, 'X', sizeof buf);
    char* fp = (char*)1;
    mark();
    r.ret = w32_GetFullPathNameA(in, n, n ? buf : 0, part ? &fp : 0);
    r.err = stub_err();
    r.out = std::string(buf, buf + (n < 1024 ? n : 1024)).c_str();
    if (part) r.out += fp == (char*)1 ? "|untouched" : fp ? fmt("|part@%d", (int)(fp - buf)) : "|part=null";
    return r;
}

static void test_paths() {
    // the user profile (C:\Users\<name>): a folder that exists, also spelt lower-case with an upper-case last folder
    char prof[MAX_PATH] = "C:\\Users\\Default";
    DWORD pn = GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH);
    if (!pn || pn >= MAX_PATH) strcpy(prof, "C:\\Users\\Default");
    std::string home = prof, home_lc = home;
    for (char& ch : home_lc) ch = (char)tolower((unsigned char)ch);
    std::string home_mc = home_lc;
    for (size_t i = home_mc.rfind('\\') + 1; i < home_mc.size(); i++)
        home_mc[i] = (char)toupper((unsigned char)home_mc[i]);
    home_mc += "\\";
    std::string home_appdata = home + "\\AppData", home_temp = home_lc + "\\appdata\\local\\temp";
    const char* cwds[] = {home_appdata.c_str(), home_mc.c_str(), "C:\\", "\\\\localhost\\c$\\Users"};
    char saved[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, saved);
    SetEnvironmentVariableA("=C:", 0);          // (a shell may have set it: "C:x" from a UNC directory would read it)
    for (const char* c : cwds) {
        if (!SetCurrentDirectoryA(c)) {
            printf("note: can't enter %s; skipped\n", c);
            continue;
        }
        for (const char* p : k_paths) {
            check(fmt("GetFullPathNameA(\"%s\") in %s", p, c), fullpath_real(p, 1024, true), fullpath_stub(p, 1024, true));
        }
        for (int len = 250; len <= 264; len++) {
            std::string a = "c:\\", b, d = "c:\\x\\";
            while (a.size() < (size_t)len) a += 'a';
            while (b.size() + strlen(c) + 1 < (size_t)len) b += 'b';
            while (d.size() + 3 < (size_t)len) d += "..\\";
            while (d.size() < (size_t)len) d += 'd';
            for (const std::string* s : {&a, &b, &d})
                check(fmt("GetFullPathNameA(%u chars \"%.12s...\") in %s", (unsigned)s->size(), s->c_str(), c),
                      fullpath_real(s->c_str(), 1024, true), fullpath_stub(s->c_str(), 1024, true));
        }
    }
    SetCurrentDirectoryA("C:\\Users");
    for (DWORD n : {0u, 1u, 5u, 9u, 10u, 11u, 12u})
        for (bool part : {false, true}) {
            check(fmt("GetFullPathNameA(\"player\", %u)", n), fullpath_real("player", n, part), fullpath_stub("player", n, part));
            check(fmt("GetFullPathNameA(\"c:\\\\\", %u)", n), fullpath_real("c:\\", n, part), fullpath_stub("c:\\", n, part));
        }
    // another drive's current directory: its "=X:" variable when that names a folder
    struct { const char* var; const char* val; const char* in; } envs[] = {
        {"=D:", "D:\\games", "D:x"}, {"=E:", "C:\\Windows", "E:x"}, {"=E:", "C:\\Windows", "E:"}, {"=E:", "C:\\Windows", "e:..\\y"},
        {"=E:", "C:\\nonexist", "E:x"}, {"=E:", "Q:\\x", "E:x"}, {"=E:", "C:\\Windows\\notepad.exe", "E:x"}};
    for (auto& v : envs) {
        SetEnvironmentVariableA(v.var, v.val);
        check(fmt("GetFullPathNameA(\"%s\") with %s%s", v.in, v.var, v.val), fullpath_real(v.in, 1024, true),
              fullpath_stub(v.in, 1024, true));
        SetEnvironmentVariableA(v.var, 0);
    }

    // the current directory
    const char* dirs[] = {"C:\\Users", home_mc.c_str(), "c:/users/./", "C:\\Windows\\..\\Users", "..", ".", "",
                          "C:\\nonexist", "C:\\nonexist\\x", "C:\\Windows\\notepad.exe", "C:\\Windows\\notepad.exe\\x",
                          "\\", "C:", "Users", home_temp.c_str(), "C:\\USERS\\.\\", "C:\\x*",
                          "\\\\localhost\\c$\\Users", "C:\\Users ", "C:\\Users."};
    for (const char* d : dirs) {
        R a, b;
        SetCurrentDirectoryA(prof);
        mark();
        a.ret = SetCurrentDirectoryA(d);
        a.err = real_err();
        char buf[MAX_PATH] = "";
        GetCurrentDirectoryA(MAX_PATH, buf);
        a.out = buf;
        SetCurrentDirectoryA(prof);
        mark();
        b.ret = w32_SetCurrentDirectoryA(d);
        b.err = stub_err();
        buf[0] = 0;
        w32_GetCurrentDirectoryA(MAX_PATH, buf);
        b.out = buf;
        check(fmt("SetCurrentDirectoryA(\"%s\")", d), a, b);
    }
    SetCurrentDirectoryA(prof);
    DWORD hn = (DWORD)home.size();                // the profile path's length: an exact fit (with the NUL) and over it
    size_t cmp = hn + 5;
    for (DWORD n : {(DWORD)0, (DWORD)1, hn + 1, hn + 2, hn + 3, (DWORD)260}) {
        R a, b;
        char buf[300];
        memset(buf, 'X', sizeof buf);
        mark();
        a.ret = GetCurrentDirectoryA(n, n ? buf : 0);
        a.err = real_err();
        a.out = std::string(buf, cmp);
        memset(buf, 'X', sizeof buf);
        mark();
        b.ret = w32_GetCurrentDirectoryA(n, n ? buf : 0);
        b.err = stub_err();
        b.out = std::string(buf, cmp);
        check(fmt("GetCurrentDirectoryA(%u)", n), a, b);
    }
    // drive types
    const char* roots[] = {"C:\\", "C:", "c:\\", "c:\\windows", "Q:\\", "Q:", "C", "", "\\\\srv\\share\\", "C:/", nullptr};
    for (const char* rt : roots) {
        R a, b;
        mark();
        a.ret = GetDriveTypeA(rt);
        a.err = real_err();
        mark();
        b.ret = w32_GetDriveTypeA(rt);
        b.err = stub_err();
        check(fmt("GetDriveTypeA(%s)", rt ? fmt("\"%s\"", rt).c_str() : "null"), a, b);
    }
    SetCurrentDirectoryA(saved);
}

// ---- CreateFileA ---------------------------------------------------------------------------------------------------
static R create_real(const std::string& p, DWORD acc, DWORD share, DWORD disp, DWORD flags, HANDLE* out = 0) {
    R r;
    mark();
    HANDLE h = CreateFileA(p.c_str(), acc, share, 0, disp, flags, 0);
    r.err = real_err();
    r.ret = h != INVALID_HANDLE_VALUE;
    if (out) *out = h;
    else if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return r;
}
static R create_stub(const std::string& p, DWORD acc, DWORD share, DWORD disp, DWORD flags, uint32_t* out = 0) {
    R r;
    mark();
    uint32_t h = w32_CreateFileA(p.c_str(), acc, share, 0, disp, flags, 0);
    r.err = stub_err();
    r.ret = h != 0xFFFFFFFFu;
    if (out) *out = h;
    else if (h != 0xFFFFFFFFu) w32_CloseHandle(h);
    return r;
}

static void test_create() {
    const DWORD accs[] = {GENERIC_READ, GENERIC_WRITE, GENERIC_READ | GENERIC_WRITE, 0, FILE_READ_DATA,
                          FILE_APPEND_DATA, DELETE};
    const char* states[] = {"missing", "file", "readonly", "hidden", "dir", "noparent"};
    for (int st = 0; st < 6; st++)
        for (DWORD disp = 0; disp <= 6; disp++)
            for (DWORD acc : accs)
                for (DWORD flags : {(DWORD)FILE_ATTRIBUTE_NORMAL, (DWORD)FILE_ATTRIBUTE_READONLY,
                                    (DWORD)(FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN),
                                    (DWORD)FILE_FLAG_BACKUP_SEMANTICS}) {
                    const std::string name = fmt("c%d_%lu_%lx_%lx", st, disp, acc, flags);
                    Twin t = twin(name.c_str(), [&](const std::string& d) {
                        if (st == 1) put_file(d + "\\f", "hello");
                        if (st == 2) put_file(d + "\\f", "hello", FILE_ATTRIBUTE_READONLY);
                        if (st == 3) put_file(d + "\\f", "hello", FILE_ATTRIBUTE_HIDDEN);
                        if (st == 4) CreateDirectoryA((d + "\\f").c_str(), 0);
                    });
                    const char* leaf = st == 5 ? "\\nope\\f" : "\\f";
                    const std::string what = fmt("CreateFileA(%s, disp %lu, access %lx, flags %lx)", states[st], disp, acc, flags);
                    check(what, create_real(t.real + leaf, acc, FILE_SHARE_READ, disp, flags),
                          create_stub(t.stub + leaf, acc, FILE_SHARE_READ, disp, flags));
                    same_state(what, t);
                    rm_tree(t.real);
                    rm_tree(t.stub);
                }
    Twin t = twin("cmisc", [](const std::string& d) { put_file(d + "\\f", "x"); });
    const char* odd[] = {"\\f\\", "\\*.x", "\\f?", "\\a<b", "\\a>b", "\\a|b", "\\a\"b", "\\f.", "\\f ", "\\F", "\\f\\..\\f",
                         "\\nope\\..\\f", "\\"};
    for (const char* o : odd)
        for (DWORD disp : {(DWORD)OPEN_EXISTING, (DWORD)OPEN_ALWAYS})
            check(fmt("CreateFileA(\"<dir>%s\", disp %lu)", o, disp), create_real(t.real + o, GENERIC_READ, 1, disp, 0),
                  create_stub(t.stub + o, GENERIC_READ, 1, disp, 0));
    same_state("CreateFileA odd names", t);
    check("CreateFileA(null)", create_real("", 0, 0, 0, 0), create_stub("", 0, 0, 0, 0));
    {
        R a, b;
        mark();
        a.ret = CreateFileA(0, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0) != INVALID_HANDLE_VALUE;
        a.err = real_err();
        mark();
        b.ret = w32_CreateFileA(0, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0) != 0xFFFFFFFFu;
        b.err = stub_err();
        check("CreateFileA(NULL)", a, b);
    }
    check("CreateFileA(\\\\.\\VEXED.VXD) (the VxD)",
          create_real("\\\\.\\VEXED.VXD", GENERIC_READ | GENERIC_WRITE, 3, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE),
          create_stub("\\\\.\\VEXED.VXD", GENERIC_READ | GENERIC_WRITE, 3, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE));
    check("CreateFileA(mailslot client, none there)",
          create_real("\\\\.\\mailslot\\mgi\\w32test", GENERIC_WRITE, 1, OPEN_EXISTING, 0x80),
          create_stub("\\\\.\\mailslot\\mgi\\w32test", GENERIC_WRITE, 1, OPEN_EXISTING, 0x80));
    // the game's own opens (krn_file.cpp), on twin files
    Twin g = twin("cgame", [](const std::string& d) { put_file(d + "\\in.txt", "line1\r\nline2\r\n"); });
    struct { const char* name; DWORD acc, share, disp, flags; } game[] = {
        {"\\new.dat", GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL},            // FileCreate
        {"\\in.txt", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY | FILE_FLAG_SEQUENTIAL_SCAN},   // FileOpen
        {"\\app.log", GENERIC_WRITE, FILE_SHARE_READ, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL},              // FileAppend
        {"\\app.log", GENERIC_WRITE, FILE_SHARE_READ, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL},              // (again: exists)
        {"\\in.txt", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS},   // FileOpenWritable
        {"\\tmp.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE},
        {"\\crt.txt", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL},   // fopen "a+"
        {"\\crt.txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL},   // fopen "w"
        {"\\in.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL},     // fopen "r"
    };
    for (auto& c : game) {
        check(fmt("CreateFileA game open %s %lx/%lx/%lu/%lx", c.name, c.acc, c.share, c.disp, c.flags),
              create_real(g.real + c.name, c.acc, c.share, c.disp, c.flags),
              create_stub(g.stub + c.name, c.acc, c.share, c.disp, c.flags));
    }
    same_state("the game's opens", g);
}

// ---- sharing ---------------------------------------------------------------------------------------------------------
static void test_sharing() {
    const DWORD accs[] = {GENERIC_READ, GENERIC_WRITE, GENERIC_READ | GENERIC_WRITE, 0, DELETE};
    const DWORD shares[] = {0, FILE_SHARE_READ, FILE_SHARE_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, 7};
    Twin t = twin("share", [](const std::string& d) { put_file(d + "\\f", "data"); });
    for (DWORD a1 : accs)
        for (DWORD s1 : shares)
            for (DWORD a2 : accs)
                for (DWORD s2 : shares) {
                    HANDLE h1;
                    uint32_t u1;
                    const R r1 = create_real(t.real + "\\f", a1, s1, OPEN_EXISTING, 0, &h1);
                    const R u = create_stub(t.stub + "\\f", a1, s1, OPEN_EXISTING, 0, &u1);
                    check(fmt("share: first open %lx/%lx", a1, s1), r1, u);
                    check(fmt("share: %lx/%lx then %lx/%lx", a1, s1, a2, s2),
                          create_real(t.real + "\\f", a2, s2, OPEN_EXISTING, 0),
                          create_stub(t.stub + "\\f", a2, s2, OPEN_EXISTING, 0));
                    if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);
                    if (u1 != 0xFFFFFFFFu) w32_CloseHandle(u1);
                }
    // delete / copy / truncate against an open file
    for (DWORD s1 : shares) {
        Twin d = twin("sharedel", [](const std::string& x) {
            put_file(x + "\\f", "data");
            put_file(x + "\\g", "other");
        });
        HANDLE h1;
        uint32_t u1;
        create_real(d.real + "\\f", GENERIC_READ, s1, OPEN_EXISTING, 0, &h1);
        create_stub(d.stub + "\\f", GENERIC_READ, s1, OPEN_EXISTING, 0, &u1);
        R a, b;
        mark();
        a.ret = CopyFileA((d.real + "\\g").c_str(), (d.real + "\\f").c_str(), FALSE);
        a.err = real_err();
        mark();
        b.ret = w32_CopyFileA((d.stub + "\\g").c_str(), (d.stub + "\\f").c_str(), FALSE);
        b.err = stub_err();
        check(fmt("CopyFileA onto a file open %lx", s1), a, b);
        mark();
        a.ret = CopyFileA((d.real + "\\f").c_str(), (d.real + "\\h").c_str(), FALSE);
        a.err = real_err();
        mark();
        b.ret = w32_CopyFileA((d.stub + "\\f").c_str(), (d.stub + "\\h").c_str(), FALSE);
        b.err = stub_err();
        check(fmt("CopyFileA from a file open %lx", s1), a, b);
        check(fmt("CREATE_ALWAYS on a file open %lx", s1), create_real(d.real + "\\f", GENERIC_WRITE, 7, CREATE_ALWAYS, 0),
              create_stub(d.stub + "\\f", GENERIC_WRITE, 7, CREATE_ALWAYS, 0));
        mark();
        a.ret = DeleteFileA((d.real + "\\f").c_str());
        a.err = real_err();
        mark();
        b.ret = w32_DeleteFileA((d.stub + "\\f").c_str());
        b.err = stub_err();
        check(fmt("DeleteFileA of a file open %lx", s1), a, b);
        if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);
        if (u1 != 0xFFFFFFFFu) w32_CloseHandle(u1);
        same_state(fmt("delete/copy against an open file %lx", s1), d);
    }
    // CopyFileA's own opens: the source shared how, the copy shared how
    for (DWORD a1 : {(DWORD)GENERIC_WRITE, (DWORD)(GENERIC_READ | GENERIC_WRITE), (DWORD)DELETE, (DWORD)GENERIC_READ})
        for (DWORD s1 : {7u, 3u, 5u, 6u, 1u, 2u}) {
            Twin d = twin("sharecopy", [](const std::string& x) {
                put_file(x + "\\f", "data");
                put_file(x + "\\g", "other");
            });
            for (int dir = 0; dir < 2; dir++) {
                HANDLE h1;
                uint32_t u1;
                create_real(d.real + "\\f", a1, s1, OPEN_EXISTING, 0, &h1);
                create_stub(d.stub + "\\f", a1, s1, OPEN_EXISTING, 0, &u1);
                const std::string from = dir ? "\\f" : "\\g", to = dir ? "\\h" : "\\f";
                R a, b;
                mark();
                a.ret = CopyFileA((d.real + from).c_str(), (d.real + to).c_str(), FALSE);
                a.err = real_err();
                mark();
                b.ret = w32_CopyFileA((d.stub + from).c_str(), (d.stub + to).c_str(), FALSE);
                b.err = stub_err();
                check(fmt("CopyFileA %s a file open %lx/%lx", dir ? "from" : "onto", a1, s1), a, b);
                if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);
                if (u1 != 0xFFFFFFFFu) w32_CloseHandle(u1);
            }
            same_state(fmt("copies against an open %lx/%lx", a1, s1), d);
        }
    // FILE_FLAG_DELETE_ON_CLOSE: gone when the last handle (a duplicate too) closes
    Twin d = twin("delclose", [](const std::string&) {});
    HANDLE h1, h2;
    uint32_t u1, u2;
    check("delete-on-close: create", create_real(d.real + "\\t", GENERIC_READ | GENERIC_WRITE, 1, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, &h1),
          create_stub(d.stub + "\\t", GENERIC_READ | GENERIC_WRITE, 1, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, &u1));
    check("delete-on-close: a second open without FILE_SHARE_DELETE", create_real(d.real + "\\t", GENERIC_READ, 7, OPEN_EXISTING, 0),
          create_stub(d.stub + "\\t", GENERIC_READ, 7, OPEN_EXISTING, 0));
    DuplicateHandle(GetCurrentProcess(), h1, GetCurrentProcess(), &h2, 0, FALSE, DUPLICATE_SAME_ACCESS);
    w32_DuplicateHandle(0xFFFFFFFFu, u1, 0xFFFFFFFFu, &u2, 0, 0, DUPLICATE_SAME_ACCESS);
    CloseHandle(h1);
    w32_CloseHandle(u1);
    same_state("delete-on-close: one of two handles closed", d);
    CloseHandle(h2);
    w32_CloseHandle(u2);
    same_state("delete-on-close: both closed", d);
}

// ---- reading, writing, seeking -------------------------------------------------------------------------------------------
struct Io {
    HANDLE h;
    uint32_t u;
};
static void io_step(const std::string& what, Io& io, const std::function<R(bool real)>& f) { check(what, f(true), f(false)); }

static void test_io() {
    Twin t = twin("io", [](const std::string& d) {
        put_file(d + "\\f", "0123456789abcdefghij");
        put_file(d + "\\ro", "readonly data");
    });
    Io rw, rd, wr;
    create_real(t.real + "\\f", GENERIC_READ | GENERIC_WRITE, 1, OPEN_EXISTING, 0, &rw.h);
    create_stub(t.stub + "\\f", GENERIC_READ | GENERIC_WRITE, 1, OPEN_EXISTING, 0, &rw.u);
    create_real(t.real + "\\ro", GENERIC_READ, 1, OPEN_EXISTING, 0, &rd.h);
    create_stub(t.stub + "\\ro", GENERIC_READ, 1, OPEN_EXISTING, 0, &rd.u);
    create_real(t.real + "\\wo", GENERIC_WRITE, 1, CREATE_ALWAYS, 0, &wr.h);
    create_stub(t.stub + "\\wo", GENERIC_WRITE, 1, CREATE_ALWAYS, 0, &wr.u);
    auto read = [](Io& io, DWORD n, bool ov_at = false, DWORD at = 0) {
        return [&io, n, ov_at, at](bool real) {
            R r;
            char buf[64];
            memset(buf, '.', sizeof buf);
            uint32_t got = 0xCCCCCCCCu;
            OVERLAPPED ov;
            memset(&ov, 0, sizeof ov);
            ov.Offset = at;
            mark();
            if (real) {
                DWORD g = 0xCCCCCCCCu;
                r.ret = ReadFile(io.h, buf, n, &g, ov_at ? &ov : 0);
                r.err = real_err();
                got = g;
            } else {
                r.ret = w32_ReadFile(io.u, buf, n, &got, ov_at ? (w32::Overlapped*)&ov : 0);
                r.err = stub_err();
            }
            r.out = fmt("got=%x ", got) + std::string(buf, 32);
            if (ov_at) r.out += fmt(" ov=%lx/%lx", (unsigned long)ov.Internal, (unsigned long)ov.InternalHigh);
            return r;
        };
    };
    auto write = [](Io& io, const char* s) {
        return [&io, s](bool real) {
            R r;
            uint32_t put = 0xCCCCCCCCu;
            mark();
            if (real) {
                DWORD p = 0xCCCCCCCCu;
                r.ret = WriteFile(io.h, s, (DWORD)strlen(s), &p, 0);
                r.err = real_err();
                put = p;
            } else {
                r.ret = w32_WriteFile(io.u, s, (uint32_t)strlen(s), &put, 0);
                r.err = stub_err();
            }
            r.out = fmt("put=%x", put);
            return r;
        };
    };
    auto seek = [](Io& io, LONG d, bool use_high, LONG high, DWORD method) {
        return [&io, d, use_high, high, method](bool real) {
            R r;
            LONG hi = high;
            mark();
            if (real) {
                r.ret = SetFilePointer(io.h, d, use_high ? &hi : 0, method);
                r.err = real_err();
            } else {
                int32_t h2 = high;
                r.ret = w32_SetFilePointer(io.u, d, use_high ? &h2 : 0, method);
                r.err = stub_err();
                hi = h2;
            }
            r.out = fmt("high=%lx", hi);
            return r;
        };
    };
    auto size = [](Io& io, bool use_high) {
        return [&io, use_high](bool real) {
            R r;
            DWORD hi = 0xCCCCCCCCu;
            uint32_t h2 = 0xCCCCCCCCu;
            mark();
            if (real) {
                r.ret = GetFileSize(io.h, use_high ? &hi : 0);
                r.err = real_err();
            } else {
                r.ret = w32_GetFileSize(io.u, use_high ? &h2 : 0);
                r.err = stub_err();
                hi = h2;
            }
            r.out = fmt("high=%lx", hi);
            return r;
        };
    };
    auto simple = [](Io& io, int which) {
        return [&io, which](bool real) {
            R r;
            mark();
            if (real) {
                r.ret = which == 0 ? SetEndOfFile(io.h) : which == 1 ? FlushFileBuffers(io.h) : GetFileType(io.h);
                r.err = real_err();
            } else {
                r.ret = which == 0 ? w32_SetEndOfFile(io.u) : which == 1 ? w32_FlushFileBuffers(io.u) : w32_GetFileType(io.u);
                r.err = stub_err();
            }
            return r;
        };
    };
    io_step("ReadFile 5", rw, read(rw, 5));
    io_step("SetFilePointer +3 cur", rw, seek(rw, 3, false, 0, FILE_CURRENT));
    io_step("ReadFile 4", rw, read(rw, 4));
    io_step("SetFilePointer -2 end", rw, seek(rw, -2, false, 0, FILE_END));
    io_step("ReadFile 10 (2 left)", rw, read(rw, 10));
    io_step("ReadFile at end", rw, read(rw, 10));
    io_step("SetFilePointer -1 begin", rw, seek(rw, -1, false, 0, FILE_BEGIN));
    io_step("SetFilePointer -100 cur", rw, seek(rw, -100, false, 0, FILE_CURRENT));
    io_step("SetFilePointer method 3", rw, seek(rw, 0, false, 0, 3));
    io_step("SetFilePointer 0 begin, high 0", rw, seek(rw, 0, true, 0, FILE_BEGIN));
    io_step("SetFilePointer -1 begin, high -1", rw, seek(rw, -1, true, -1, FILE_BEGIN));
    io_step("SetFilePointer 5 begin, high 1 (past 4 GB)", rw, seek(rw, 5, true, 1, FILE_BEGIN));
    io_step("GetFileSize after a seek past 4 GB", rw, size(rw, true));
    io_step("SetFilePointer 0 cur (past 4 GB, no high)", rw, seek(rw, 0, false, 0, FILE_CURRENT));
    io_step("SetFilePointer 0x7fffffff begin", rw, seek(rw, 0x7fffffff, false, 0, FILE_BEGIN));
    io_step("SetFilePointer 0x7fffffff cur (no high)", rw, seek(rw, 0x7fffffff, false, 0, FILE_CURRENT));
    io_step("SetFilePointer -1 begin, high 0 (0xffffffff)", rw, seek(rw, -1, true, 0, FILE_BEGIN));
    io_step("SetFilePointer 0 cur at 0xffffffff, high", rw, seek(rw, 0, true, 0, FILE_CURRENT));
    io_step("SetFilePointer 30 begin (past the end)", rw, seek(rw, 30, false, 0, FILE_BEGIN));
    io_step("ReadFile past the end", rw, read(rw, 4));
    io_step("WriteFile past the end", rw, write(rw, "XY"));
    io_step("GetFileSize", rw, size(rw, false));
    io_step("GetFileSize (high)", rw, size(rw, true));
    io_step("SetFilePointer 10 begin", rw, seek(rw, 10, false, 0, FILE_BEGIN));
    io_step("SetEndOfFile at 10", rw, simple(rw, 0));
    io_step("GetFileSize after SetEndOfFile", rw, size(rw, true));
    io_step("WriteFile 0 bytes", rw, write(rw, ""));
    io_step("FlushFileBuffers", rw, simple(rw, 1));
    io_step("GetFileType (disk)", rw, simple(rw, 2));
    io_step("ReadFile at offset 2 (OVERLAPPED)", rw, read(rw, 3, true, 2));
    io_step("ReadFile after an OVERLAPPED read", rw, read(rw, 3));
    io_step("WriteFile on a read-only handle", rd, write(rd, "no"));
    io_step("SetEndOfFile on a read-only handle", rd, simple(rd, 0));
    io_step("FlushFileBuffers on a read-only handle", rd, simple(rd, 1));
    io_step("ReadFile on a write-only handle", wr, read(wr, 4));
    io_step("WriteFile on a write-only handle", wr, write(wr, "abc"));
    io_step("GetFileSize of a write-only handle", wr, size(wr, true));
    CloseHandle(rw.h);
    w32_CloseHandle(rw.u);
    io_step("ReadFile on a closed handle", rw, read(rw, 4));
    io_step("WriteFile on a closed handle", rw, write(rw, "x"));
    io_step("SetFilePointer on a closed handle", rw, seek(rw, 0, false, 0, FILE_BEGIN));
    io_step("GetFileSize on a closed handle", rw, size(rw, true));
    io_step("SetEndOfFile on a closed handle", rw, simple(rw, 0));
    io_step("FlushFileBuffers on a closed handle", rw, simple(rw, 1));
    io_step("GetFileType on a closed handle", rw, simple(rw, 2));
    Io zero{0, 0}, inval{INVALID_HANDLE_VALUE, 0xFFFFFFFFu};
    io_step("ReadFile on handle 0", zero, read(zero, 4));
    io_step("GetFileType on handle 0", zero, simple(zero, 2));
    io_step("GetFileType on -1", inval, simple(inval, 2));
    io_step("GetFileSize on -1", inval, size(inval, true));
    CloseHandle(rd.h);
    w32_CloseHandle(rd.u);
    CloseHandle(wr.h);
    w32_CloseHandle(wr.u);
    same_state("after the I/O", t);

    // the CRT's text "r+" open: lseek(-1, SEEK_END) of an empty file is ERROR_NEGATIVE_SEEK (131)
    Twin e = twin("empty", [](const std::string& d) { put_file(d + "\\e", ""); });
    Io em;
    create_real(e.real + "\\e", GENERIC_READ | GENERIC_WRITE, 3, OPEN_EXISTING, 0, &em.h);
    create_stub(e.stub + "\\e", GENERIC_READ | GENERIC_WRITE, 3, OPEN_EXISTING, 0, &em.u);
    io_step("SetFilePointer -1 end of an empty file", em, seek(em, -1, false, 0, FILE_END));
    io_step("GetFileSize of an empty file", em, size(em, true));
    CloseHandle(em.h);
    w32_CloseHandle(em.u);

    // the handles
    auto close_r = [](HANDLE h) {
        R r;
        mark();
        r.ret = CloseHandle(h);
        r.err = real_err();
        return r;
    };
    auto close_s = [](uint32_t h) {
        R r;
        mark();
        r.ret = w32_CloseHandle(h);
        r.err = stub_err();
        return r;
    };
    check("CloseHandle(closed)", close_r(rw.h), close_s(rw.u));
    check("CloseHandle(0)", close_r(0), close_s(0));
    check("CloseHandle(GetCurrentProcess())", close_r(GetCurrentProcess()), close_s(0xFFFFFFFFu));
    check("CloseHandle(GetCurrentThread())", close_r(GetCurrentThread()), close_s(0xFFFFFFFEu));
    {
        WIN32_FIND_DATAA fd;
        HANDLE fr = FindFirstFileA((t.real + "\\*").c_str(), &fd);
        uint32_t fs = w32_FindFirstFileA((t.stub + "\\*").c_str(), (w32::FindDataA*)&fd);
        check("CloseHandle(a find handle)", close_r(fr), close_s(fs));
        R a, b;
        mark();
        a.ret = FindClose(fr);
        a.err = real_err();
        mark();
        b.ret = w32_FindClose(fs);
        b.err = stub_err();
        check("FindClose after CloseHandle refused it", a, b);
        // (FindClose twice: Windows reads its freed listing -- ERROR_NOACCESS here -- the stand-in says
        // ERROR_INVALID_HANDLE; not compared)
        mark();
        b.ret = w32_FindClose(fs);
        expect("FindClose twice fails", !b.ret && stub_err() == w32::ERR_INVALID_HANDLE);
    }
    // DuplicateHandle: the duplicate shares the position; DUPLICATE_CLOSE_SOURCE; bad handles
    {
        Twin d = twin("dup", [](const std::string& x) { put_file(x + "\\f", "0123456789"); });
        Io a, b;
        create_real(d.real + "\\f", GENERIC_READ | GENERIC_WRITE, 1, OPEN_EXISTING, 0, &a.h);
        create_stub(d.stub + "\\f", GENERIC_READ | GENERIC_WRITE, 1, OPEN_EXISTING, 0, &a.u);
        auto dup = [&](Io& src, Io* dst, DWORD opts, HANDLE rp, uint32_t sp, bool null_target = false) {
            return [&, dst, opts, rp, sp, null_target](bool real) {
                R r;
                mark();
                if (real) {
                    HANDLE out = (HANDLE)(uintptr_t)0xCCCCCCCC;
                    r.ret = DuplicateHandle(rp, src.h, rp, null_target ? 0 : &out, 0, FALSE, opts);
                    r.err = real_err();
                    if (dst) dst->h = out;
                    r.out = out == (HANDLE)(uintptr_t)0xCCCCCCCC ? "untouched" : out ? "set" : "null";
                } else {
                    uint32_t out = 0xCCCCCCCCu;
                    r.ret = w32_DuplicateHandle(sp, src.u, sp, null_target ? 0 : &out, 0, 0, opts);
                    r.err = stub_err();
                    if (dst) dst->u = out;
                    r.out = out == 0xCCCCCCCCu ? "untouched" : out ? "set" : "null";
                }
                return r;
            };
        };
        io_step("DuplicateHandle (same access)", a, dup(a, &b, DUPLICATE_SAME_ACCESS, GetCurrentProcess(), 0xFFFFFFFFu));
        io_step("ReadFile 3 on the original", a, read(a, 3));
        io_step("ReadFile 3 on the duplicate (shared position)", b, read(b, 3));
        io_step("DuplicateHandle with a bad process handle", a, dup(a, 0, DUPLICATE_SAME_ACCESS, (HANDLE)(uintptr_t)0x1234, 0x1234));
        io_step("DuplicateHandle to a null target", a, dup(a, 0, DUPLICATE_SAME_ACCESS, GetCurrentProcess(), 0xFFFFFFFFu, true));
        Io c;
        io_step("DuplicateHandle + DUPLICATE_CLOSE_SOURCE", b, dup(b, &c, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE,
                                                                 GetCurrentProcess(), 0xFFFFFFFFu));
        // (Windows hands the closed source's value straight to the duplicate, so a stale handle reaches it there;
        // the stand-ins never reuse a value soon: the stale handle fails)
        io_step("ReadFile on the new one", c, read(c, 3));
        {
            uint32_t got;
            char x[4];
            mark();
            expect("ReadFile on the closed source fails", !w32_ReadFile(b.u, x, 3, &got, 0) && stub_err() == 6);
            uint32_t out = 5;
            expect("DuplicateHandle of a closed handle fails, target 0",
                   !w32_DuplicateHandle(0xFFFFFFFFu, b.u, 0xFFFFFFFFu, &out, 0, 0, 2) && out == 0 && stub_err() == 6);
        }
        io_step("FileFlush's dup + close (krn_file)", a, [&](bool real) {
            R r;
            mark();
            if (real) {
                HANDLE x;
                r.ret = DuplicateHandle(GetCurrentProcess(), a.h, GetCurrentProcess(), &x, 0, FALSE, DUPLICATE_SAME_ACCESS);
                r.ret = r.ret * 2 + CloseHandle(x);
                r.err = real_err();
            } else {
                uint32_t x;
                r.ret = w32_DuplicateHandle(0xFFFFFFFFu, a.u, 0xFFFFFFFFu, &x, 0, 0, DUPLICATE_SAME_ACCESS);
                r.ret = r.ret * 2 + w32_CloseHandle(x);
                r.err = stub_err();
            }
            return r;
        });
        CloseHandle(a.h);
        w32_CloseHandle(a.u);
        CloseHandle(c.h);
        w32_CloseHandle(c.u);
        same_state("after the duplicates", d);
    }
}

// ---- delete, copy, folders, attributes, temp names --------------------------------------------------------------------
static void test_names() {
    auto both = [](const std::string& what, const Twin& t, const std::function<uint32_t(const std::string&, bool)>& f) {
        R a, b;
        mark();
        a.ret = f(t.real, true);
        a.err = real_err();
        mark();
        b.ret = f(t.stub, false);
        b.err = stub_err();
        check(what, a, b);
        return b.ret != 0;
    };
    Twin t = twin("names", [](const std::string& d) {
        put_file(d + "\\f", "file");
        put_file(d + "\\ro", "ro", FILE_ATTRIBUTE_READONLY);
        put_file(d + "\\hid", "hidden", FILE_ATTRIBUTE_HIDDEN);
        put_file(d + "\\big", std::string(200000, 'z'));
        CreateDirectoryA((d + "\\dir").c_str(), 0);
        put_file(d + "\\dir\\in", "inside");
    });
    const char* dels[] = {"f", "f", "ro", "dir", "nope", "nope\\x", "", "*", "dir\\in", "F.", "hid"};
    for (const char* n : dels)
        both(fmt("DeleteFileA(\"%s\")", n), t, [n](const std::string& d, bool real) {
            const std::string p = d + "\\" + n;
            return real ? DeleteFileA(p.c_str()) : w32_DeleteFileA(p.c_str());
        });
    same_state("after the deletes", t);

    Twin c = twin("copy", [](const std::string& d) {
        put_file(d + "\\a", "source a");
        put_file(d + "\\ro", "read-only source", FILE_ATTRIBUTE_READONLY);
        put_file(d + "\\hid", "hidden source", FILE_ATTRIBUTE_HIDDEN);
        put_file(d + "\\exists", "old");
        put_file(d + "\\rodst", "old ro", FILE_ATTRIBUTE_READONLY);
        put_file(d + "\\big", std::string(300000, 'q'));
        CreateDirectoryA((d + "\\dir").c_str(), 0);
    });
    struct { const char* a; const char* b; BOOL fail; } copies[] = {
        {"a", "new", FALSE}, {"a", "exists", TRUE}, {"a", "exists", FALSE}, {"a", "rodst", FALSE}, {"nope", "x", FALSE},
        {"a", "nodir\\x", FALSE}, {"a", "a", FALSE}, {"dir", "d2", FALSE}, {"a", "dir", FALSE}, {"ro", "ro2", FALSE},
        {"hid", "hid2", FALSE}, {"big", "big2", TRUE}, {"a", "", FALSE}, {"", "x", FALSE}};
    for (auto& k : copies) {
        const bool copied = both(fmt("CopyFileA(\"%s\", \"%s\", %d)", k.a, k.b, k.fail), c, [&k](const std::string& d, bool real) {
            const std::string a = d + "\\" + k.a, b = d + "\\" + k.b;
            return real ? CopyFileA(a.c_str(), b.c_str(), k.fail) : w32_CopyFileA(a.c_str(), b.c_str(), k.fail);
        });
        // the copy's last-write time is the source's
        WIN32_FILE_ATTRIBUTE_DATA x, y, z;
        if (copied && GetFileAttributesExA((c.real + "\\" + k.b).c_str(), GetFileExInfoStandard, &x) &&
            GetFileAttributesExA((c.stub + "\\" + k.b).c_str(), GetFileExInfoStandard, &y) &&
            GetFileAttributesExA((c.stub + "\\" + k.a).c_str(), GetFileExInfoStandard, &z) &&
            !(z.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            expect(fmt("CopyFileA(\"%s\") keeps the last-write time", k.a),
                   !memcmp(&y.ftLastWriteTime, &z.ftLastWriteTime, sizeof z.ftLastWriteTime));
            R a, b;
            a.out = fmt("attrs %lx", x.dwFileAttributes);
            b.out = fmt("attrs %lx", y.dwFileAttributes);
            check(fmt("CopyFileA(\"%s\", \"%s\") attributes", k.a, k.b), a, b);
        }
    }
    same_state("after the copies", c);

    Twin m = twin("mkdir", [](const std::string& d) { put_file(d + "\\file", "f"); });
    const char* mk[] = {"new", "new", "file", "no\\x", "new\\sub\\", "trail\\", "", "a*b", "new\\..\\new2", "new2."};
    for (const char* n : mk)
        both(fmt("CreateDirectoryA(\"%s\")", n), m, [n](const std::string& d, bool real) {
            const std::string p = d + "\\" + n;
            return real ? CreateDirectoryA(p.c_str(), 0) : w32_CreateDirectoryA(p.c_str(), 0);
        });
    same_state("after the folders", m);

    Twin s = twin("attrs", [](const std::string& d) {
        put_file(d + "\\f", "f");
        CreateDirectoryA((d + "\\dir").c_str(), 0);
    });
    struct { const char* n; DWORD a; } sa[] = {{"f", FILE_ATTRIBUTE_READONLY}, {"f", FILE_ATTRIBUTE_NORMAL},
                                               {"f", FILE_ATTRIBUTE_READONLY}, {"f", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE},
                                               {"f", FILE_ATTRIBUTE_NORMAL}, {"dir", FILE_ATTRIBUTE_READONLY},
                                               {"dir", FILE_ATTRIBUTE_NORMAL}, {"nope", FILE_ATTRIBUTE_NORMAL},
                                               {"nope\\x", FILE_ATTRIBUTE_NORMAL}, {"f", 0}};
    for (auto& k : sa) {
        both(fmt("SetFileAttributesA(\"%s\", %lx)", k.n, k.a), s, [&k](const std::string& d, bool real) {
            const std::string p = d + "\\" + k.n;
            return real ? SetFileAttributesA(p.c_str(), k.a) : w32_SetFileAttributesA(p.c_str(), k.a);
        });
        R a, b;
        a.out = attrs_of(s.real + "\\" + k.n);
        b.out = attrs_of(s.stub + "\\" + k.n);
        check(fmt("SetFileAttributesA(\"%s\", %lx): the attributes", k.n, k.a), a, b);
    }
    // the game's FileSetWritable then FileRemove of a read-only file
    both("FileSetWritable(f, 0) + FileRemove(f)", s, [](const std::string& d, bool real) {
        const std::string p = d + "\\f";
        if (real) return (uint32_t)(SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_READONLY) * 2 + DeleteFileA(p.c_str()));
        return w32_SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_READONLY) * 2 + w32_DeleteFileA(p.c_str());
    });
    same_state("after the attributes", s);

    // GetTempFileNameA
    Twin g = twin("temp", [](const std::string& d) { put_file(d + "\\file", "f"); });
    char saved[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, saved);
    struct { const char* dir; const char* pre; UINT u; } tn[] = {
        {".", "mgi", 1}, {".", "mgi", 0x12345}, {".", "mgi", 0xabc}, {".\\", "mgi", 5}, {".", "", 5}, {".", "abcdef", 5},
        {"nosuch", "mgi", 5}, {"nosuch", "mgi", 0}, {"file", "mgi", 5}, {"", "mgi", 5}, {".", "m.g", 7}, {"..", "x", 0xffff}};
    for (auto& k : tn) {
        R a, b;
        char buf[300];
        SetCurrentDirectoryA(g.real.c_str());
        memset(buf, 'X', sizeof buf);
        mark();
        a.ret = GetTempFileNameA(k.dir, k.pre, k.u, buf);
        a.err = real_err();
        a.out = a.ret ? std::string(buf) : std::string(buf, 8);
        SetCurrentDirectoryA(g.stub.c_str());
        memset(buf, 'X', sizeof buf);
        mark();
        b.ret = w32_GetTempFileNameA(k.dir, k.pre, k.u, buf);
        b.err = stub_err();
        b.out = b.ret ? std::string(buf) : std::string(buf, 8);
        check(fmt("GetTempFileNameA(\"%s\", \"%s\", %x)", k.dir, k.pre, k.u), a, b);
    }
    {
        // unique 0: a new empty file, a non-zero number, error 0 (the number is the clock's: not compared)
        char buf[300];
        SetCurrentDirectoryA(g.stub.c_str());
        mark();
        const uint32_t u = w32_GetTempFileNameA(".", "mgi", 0, buf);
        expect("GetTempFileNameA(\".\", \"mgi\", 0) makes a file", u && GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES &&
                                                                     stub_err() == 0 && get_file(buf).empty(),
               fmt("u=%u err=%u name=%s", u, stub_err(), buf));
        mark();
        const uint32_t u2 = w32_GetTempFileNameA(".", "mgi", 0, buf);
        expect("GetTempFileNameA(0) twice: two files", u2 && u2 != u);
        // FileCreateTemp's CREATE_NEW on it: ERROR_FILE_EXISTS
        SetCurrentDirectoryA(g.real.c_str());
        char rb[300];
        GetTempFileNameA(".", "mgi", 0, rb);
        R a = create_real(rb, GENERIC_READ | GENERIC_WRITE, 1, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE);
        SetCurrentDirectoryA(g.stub.c_str());
        R b = create_stub(buf, GENERIC_READ | GENERIC_WRITE, 1, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE);
        check("FileCreateTemp: CREATE_NEW on GetTempFileNameA(0)'s file", a, b);
    }
    SetCurrentDirectoryA(saved);
}

// ---- listings ----------------------------------------------------------------------------------------------------------
static std::string find_seq(const std::string& pat, bool real, uint32_t* first_err) {
    std::string s;
    w32::FindDataA fd;
    memset(&fd, 0xCC, sizeof fd);
    mark();
    uint32_t h;
    HANDLE rh = INVALID_HANDLE_VALUE;
    if (real) rh = FindFirstFileA(pat.c_str(), (WIN32_FIND_DATAA*)&fd);
    else h = w32_FindFirstFileA(pat.c_str(), &fd);
    const bool ok = real ? rh != INVALID_HANDLE_VALUE : h != 0xFFFFFFFFu;
    *first_err = real ? real_err() : stub_err();
    auto dump = [&]() {
        // not compared: dwReserved0 / dwReserved1 (Windows leaves its own stack there: they differ run to run) and
        // a folder's last-access time (a listing of the folder moves it)
        w32::FindDataA c = fd;
        c.dwReserved0 = c.dwReserved1 = 0;
        if (c.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) c.ftLastAccessTime[0] = c.ftLastAccessTime[1] = 0;
        std::string x;
        const uint8_t* b = (const uint8_t*)&c;
        for (size_t i = 0; i < sizeof fd; i++) x += fmt("%02x", b[i]);
        return std::string(fd.cFileName[0] != (char)0xCC ? fd.cFileName : "?") + "(" + fd.cAlternateFileName + ")#" + x;
    };
    if (!ok) return "FAIL " + std::string(((uint8_t*)&fd)[0] == 0xCC && ((uint8_t*)&fd)[300] == 0xCC ? "fd untouched" : "fd written");
    for (int k = 0; k < 2000; k++) {
        s += dump() + "\n";
        memset(&fd, 0xCC, sizeof fd);
        mark();
        const uint32_t more = real ? FindNextFileA(rh, (WIN32_FIND_DATAA*)&fd) : w32_FindNextFileA(h, &fd);
        const uint32_t e = real ? real_err() : stub_err();
        if (!more) {
            s += fmt("end err=%u %s", e, ((uint8_t*)&fd)[0] == 0xCC ? "fd untouched" : "fd written");
            break;
        }
        if (e != MARK) s += fmt("[next err %u]", e);
    }
    mark();
    const uint32_t c = real ? FindClose(rh) : w32_FindClose(h);
    s += fmt(" close=%u/%u", c, real ? real_err() : stub_err());
    return s;
}

static void test_find() {
    std::string d = g_base + "\\list";
    CreateDirectoryA(d.c_str(), 0);
    const char* names[] = {"abc", "abc.txt", "abc.tx", "abc.txta", "ab", "a.b.c", "x.car", "longname.car", "foo.carbak",
                           "a_b", "AB1", "Zed.CAR", "a b.car", "a~1", "noext", ".hidden", "b.c", "ac", "abcdefghi.j",
                           "ABCDEFGH.IJKL", "ab.c.d", "x..y", "\xe4.txt", "[x]", "a+b", "a,b", "a=b", "a;b", "UPPER.TXT",
                           "MiXeD.Txt", "azzaroni.car", "azzaronir.car", "dinput.dll.pre-r1", "dinput.dll.pre-h0",
                           "dinput.dll.m2-a5f33f6", "drivers.res", "drivers.res.bak", "BENCH.RPL", "dsoal-aldrv.dll",
                           "common.res", "english.lng", "bemidji.trk", "a", "z", "Z1", "_", "~", "!x", "#y", "$z", "0", "9a",
                           "\xc9t\xe9.trk", "\x9a.x", "long file name with spaces.txt", "trailing..dots", "x.y.z.w.car"};
    for (const char* n : names) put_file(d + "\\" + n, n);
    SetFileAttributesA((d + "\\.hidden").c_str(), FILE_ATTRIBUTE_HIDDEN);
    SetFileAttributesA((d + "\\b.c").c_str(), FILE_ATTRIBUTE_READONLY);
    for (const char* sd : {"sub", "sub.dir", "Config", "Config-2"}) CreateDirectoryA((d + "\\" + sd).c_str(), 0);
    put_file(d + "\\Config\\setup.ini", "[x]");
    const char* pats[] = {"*", "*.*", "*.", "*.car", "*.CAR", "?", "??", "???", "a?", "a??", "a*", "*c", "*.?", "*.??",
                          "*.???", "a?c", "ab?", "abc.*", "abc.???", "abc.?", "abc?", "abc.tx?", "*.t*", "*b*", "a*.*", "a*.",
                          "*.*.*", "?.?", "a.*", "abc", "ABC", "abc.", "abc..", "abc*", "*~1*", "LONGNA~1.CAR", "FOO~1.CAR",
                          "foo~1.car", "*.ca", "*.carb*", "x.*", "x?car", "*x", "b?c", "?b*", ".*", "*.hidden", "a.", "a.?",
                          "sub", "sub\\*", "sub\\*.*", "sub\\", "nosuch\\*", "nosuch", "nosuch.*", "*.*.", "<", ">", "\"",
                          "a<", "a>", "a\"b", "*\"*", "a.b.*", "a.b.?", "abc<", "a*b*c", "**", "*?", "?*", "*.t", "*t",
                          "x..y", "x.*.y", "x.?.y", "*..*", "*.trk", "*.res", "*.lng", "*.rpl", "*.dll", "dinput*",
                          "DINPUT~1.PRE", "*.pre", "*.m2-", "Config", "config", "Config\\*.ini", "Config-2", "CONFIG~1",
                          "*.ini", "\xe9*", "*\xe9*", "\xc9*", "\x8a.x", "long*", "LONGFI~1.TXT", "trailing..dots", "trailing.*",
                          "*.w.car", "x.y.z.w.*", ".", "..", "sub\\.", "sub\\..", "*\\*", "nosuch\\x\\*", "abc\\*", "abc\\",
                          "*.txt ", "abc ", " abc", "nosuch\\", "sub\\\\", "C:sub\\", "sub/"};
    char saved[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, saved);
    SetCurrentDirectoryA(d.c_str());
    for (const char* p : pats)
        for (bool absolute : {true, false}) {
            const std::string pat = absolute ? d + "\\" + p : std::string(p);
            R a, b;
            a.out = find_seq(pat, true, &a.err);
            b.out = find_seq(pat, false, &b.err);
            check(fmt("FindFirstFileA(%s\"%s\")", absolute ? "<dir>\\" : "", p), a, b);
        }
    const char* other[] = {"C:\\", "C:", "C:\\*", "C:\\Users", "C:\\Users\\", "C:\\Users\\.", "C:\\Users\\..", "",
                           "C:\\nosuchdir\\x\\*", "C:\\Windows\\notepad.exe\\*", "\\\\.\\x", "C:\\Windows\\System32\\drivers",
                           "C:\\PROGRA~1", "C:\\program files", "c:\\windows\\*.exe", "\\\\localhost\\c$\\Users\\*",
                           "C:\\Users\\*"};
    for (const char* p : other) {
        R a, b;
        a.out = find_seq(p, true, &a.err);
        b.out = find_seq(p, false, &b.err);
        check(fmt("FindFirstFileA(\"%s\")", p), a, b);
    }
    {
        R a, b;
        mark();
        a.ret = 0;          // (Windows takes a find handle for a pointer: a bad one crashes it; it says 6 on a closed one)
        a.err = 6;
        mark();
        b.ret = w32_FindNextFileA(0x1234, 0);
        b.err = stub_err();
        check("FindNextFileA(bad handle)", a, b);
        HANDLE fr = CreateFileA((d + "\\abc").c_str(), GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
        uint32_t fs = w32_CreateFileA((d + "\\abc").c_str(), GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
        // (Windows reads a file handle given to FindClose as its listing's address: ERROR_NOACCESS here, or worse;
        // the stand-in refuses it, ERROR_INVALID_HANDLE)
        mark();
        expect("FindClose(a file handle) fails", !w32_FindClose(fs) && stub_err() == w32::ERR_INVALID_HANDLE);
        CloseHandle(fr);
        w32_CloseHandle(fs);
    }
    SetCurrentDirectoryA(saved);

    // the Linux design, checked against this machine: the 8.3 names Windows made (numbered 1 here: each was the
    // first of its kind) and the order a listing has
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
    std::string prev;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        expect(fmt("is_short_name(\"%s\")", fd.cFileName), w32::path::is_short_name(fd.cFileName) == !fd.cAlternateFileName[0]);
        if (fd.cAlternateFileName[0] && strstr(fd.cAlternateFileName, "~1")) {
            // (a short base takes four checksum digits: Windows' aren't always reproduced -- see make_short_name --
            // so those four are left out)
            auto mask = [](std::string s) {
                const size_t t = s.find('~');
                if (t != std::string::npos && t >= 4 && t <= 6 &&
                    s.find_first_not_of("0123456789ABCDEF", t - 4) == t)
                    s.replace(t - 4, 4, "####");
                return s;
            };
            const std::string m = w32::path::make_short_name(fd.cFileName, 1);
            R a, b;
            a.out = mask(fd.cAlternateFileName);
            b.out = mask(m);
            check(fmt("make_short_name(\"%s\") (Linux)", fd.cFileName), a, b);
        }
        if (!prev.empty())
            expect(fmt("compare_names: \"%s\" before \"%s\"", prev.c_str(), fd.cFileName),
                   w32::path::compare_names(prev.c_str(), fd.cFileName) < 0);
        prev = fd.cFileName;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- mappings ------------------------------------------------------------------------------------------------------------
static void test_maps() {
    Twin t = twin("map", [](const std::string& d) {
        std::string data;
        for (int i = 0; i < 100000; i++) data += (char)('a' + i % 26);
        put_file(d + "\\f", data);
        put_file(d + "\\e", "");
    });
    struct F {
        HANDLE h;
        uint32_t u;
    };
    F ro, rw, em, wo;
    create_real(t.real + "\\f", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &ro.h);
    create_stub(t.stub + "\\f", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &ro.u);
    create_real(t.real + "\\e", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &em.h);
    create_stub(t.stub + "\\e", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0, &em.u);
    create_real(t.real + "\\w", GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, 0, &rw.h);
    create_stub(t.stub + "\\w", GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, 0, &rw.u);
    create_real(t.real + "\\wo", GENERIC_WRITE, 0, CREATE_ALWAYS, 0, &wo.h);
    create_stub(t.stub + "\\wo", GENERIC_WRITE, 0, CREATE_ALWAYS, 0, &wo.u);
    auto cmap = [](F& f, DWORD prot, DWORD hi, DWORD lo, const char* name, F* out, bool pagefile = false) {
        R a, b;
        mark();
        HANDLE m = CreateFileMappingA(pagefile ? INVALID_HANDLE_VALUE : f.h, 0, prot, hi, lo, name);
        a.ret = m != 0;
        a.err = real_err();
        mark();
        uint32_t u = w32_CreateFileMappingA(pagefile ? 0xFFFFFFFFu : f.u, 0, prot, hi, lo, name);
        b.ret = u != 0;
        b.err = stub_err();
        if (out) {
            out->h = m;
            out->u = u;
        } else {
            if (m) CloseHandle(m);
            if (u) w32_CloseHandle(u);
        }
        check(fmt("CreateFileMappingA(prot %lx, size %lx:%lx, %s%s)", prot, hi, lo, name ? name : "unnamed",
                  pagefile ? ", pagefile" : ""), a, b);
    };
    F m1, m2, mp;
    cmap(ro, PAGE_READONLY, 0, 0, 0, &m1);
    cmap(em, PAGE_READONLY, 0, 0, 0, 0);
    cmap(ro, PAGE_READONLY, 0, 200000, 0, 0);
    cmap(ro, PAGE_READWRITE, 0, 0, 0, 0);
    cmap(ro, 0x1234, 0, 0, 0, 0);
    cmap(wo, PAGE_READONLY, 0, 0, 0, 0);
    cmap(ro, PAGE_READONLY, 0, 0, 0, 0, true);
    cmap(ro, PAGE_READWRITE, 0, 0x20000, 0, &mp, true);
    cmap(rw, PAGE_READWRITE, 0, 0x30000, 0, 0);                    // a writable mapping grows the file
    {
        F bad{(HANDLE)(uintptr_t)0x5678, 0x5678};
        cmap(bad, PAGE_READONLY, 0, 0, 0, 0);
    }
    // a named mapping: the second create of the name gets the same one and ERROR_ALREADY_EXISTS (get_mapfile_ptr)
    cmap(ro, PAGE_READONLY, 0, 0, "w32_file_test_map", &m2);
    cmap(ro, PAGE_READONLY, 0, 0, "w32_file_test_map", 0);
    auto view = [](F& m, DWORD acc, DWORD hi, DWORD lo, DWORD n, void** pr, void** ps) {
        R a, b;
        mark();
        void* p = MapViewOfFile(m.h, acc, hi, lo, n);
        a.ret = p != 0;
        a.err = real_err();
        mark();
        void* q = w32_MapViewOfFile(m.u, acc, hi, lo, n);
        b.ret = q != 0;
        b.err = stub_err();
        if (p && q) {
            a.out = std::string((const char*)p, 40);
            b.out = std::string((const char*)q, 40);
        }
        check(fmt("MapViewOfFile(access %lx, offset %lx:%lx, %lu)", acc, hi, lo, n), a, b);
        if (pr) *pr = p;
        else if (p) UnmapViewOfFile(p);
        if (ps) *ps = q;
        else if (q) w32_UnmapViewOfFile(q);
    };
    void *p1, *q1, *p2, *q2;
    view(m1, FILE_MAP_READ, 0, 0, 0, &p1, &q1);
    expect("MapViewOfFile: the whole file", p1 && q1 && !memcmp(p1, q1, 100000));
    view(m1, FILE_MAP_READ, 0, 0x10000, 0, 0, 0);
    view(m1, FILE_MAP_READ, 0, 0x100, 0, 0, 0);
    view(m1, FILE_MAP_READ, 0, 0, 200000, 0, 0);
    view(m1, FILE_MAP_WRITE, 0, 0, 0, 0, 0);
    view(m1, FILE_MAP_COPY, 0, 0, 0, 0, 0);
    view(m1, FILE_MAP_READ, 0, 0x20000, 0, 0, 0);
    view(m2, FILE_MAP_READ, 0, 0, 0, &p2, &q2);
    view(mp, FILE_MAP_WRITE, 0, 0, 0, 0, 0);
    {
        F bad{(HANDLE)(uintptr_t)0x5678, 0x5678};
        view(bad, FILE_MAP_READ, 0, 0, 0, 0, 0);
    }
    // the views outlive their mapping and file handles (get_mapfile_ptr closes them while the view is used)
    CloseHandle(m1.h);
    w32_CloseHandle(m1.u);
    CloseHandle(ro.h);
    w32_CloseHandle(ro.u);
    expect("a view after its mapping and file are closed", !memcmp(p1, q1, 100000));
    auto unmap = [](const void* p, const void* q, const char* what) {
        R a, b;
        mark();
        a.ret = UnmapViewOfFile(p);
        a.err = real_err();
        mark();
        b.ret = w32_UnmapViewOfFile(q);
        b.err = stub_err();
        check(fmt("UnmapViewOfFile(%s)", what), a, b);
    };
    unmap((const char*)p2 + 0x10, (const char*)q2 + 0x10, "inside the view");
    unmap(p1, q1, "the view");
    unmap(p1, q1, "the view again");
    unmap(0, 0, "null");
    CloseHandle(m2.h);
    w32_CloseHandle(m2.u);
    CloseHandle(mp.h);
    w32_CloseHandle(mp.u);
    for (F* f : {&em, &rw, &wo}) {
        CloseHandle(f->h);
        w32_CloseHandle(f->u);
    }
    same_state("after the mappings", t);
    expect("every file, listing and mapping closed", w32::file_open_count() == 0, fmt("(%u open)", w32::file_open_count()));
}

// ---- the standard handles, the console, the not-available ones -----------------------------------------------------------
static void test_std() {
    for (DWORD w : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE, (DWORD)5, (DWORD)-13}) {
        R a, b;
        mark();
        HANDLE h = GetStdHandle(w);
        a.err = real_err();
        a.ret = h == INVALID_HANDLE_VALUE ? 2 : h == 0 ? 0 : 1;
        a.out = h && h != INVALID_HANDLE_VALUE ? fmt("type %lu", GetFileType(h)) : "";
        mark();
        uint32_t u = w32_GetStdHandle(w);
        b.err = stub_err();
        b.ret = u == 0xFFFFFFFFu ? 2 : u == 0 ? 0 : 1;
        b.out = u && u != 0xFFFFFFFFu ? fmt("type %u", w32_GetFileType(u)) : "";
        check(fmt("GetStdHandle(%ld) (and its GetFileType)", (long)w), a, b);
        expect(fmt("GetStdHandle(%ld) twice: the same handle", (long)w), w32_GetStdHandle(w) == u);
    }
    {
        R a, b;
        HANDLE old = GetStdHandle(STD_ERROR_HANDLE);
        mark();
        a.ret = SetStdHandle(STD_ERROR_HANDLE, (HANDLE)(uintptr_t)0x1234);
        a.err = real_err();
        a.out = fmt("%p", (void*)GetStdHandle(STD_ERROR_HANDLE));
        SetStdHandle(STD_ERROR_HANDLE, old);
        const uint32_t uold = w32_GetStdHandle(STD_ERROR_HANDLE);
        mark();
        b.ret = w32_SetStdHandle(STD_ERROR_HANDLE, 0x1234);
        b.err = stub_err();
        b.out = fmt("%p", (void*)(uintptr_t)w32_GetStdHandle(STD_ERROR_HANDLE));
        w32_SetStdHandle(STD_ERROR_HANDLE, uold);
        check("SetStdHandle(STD_ERROR_HANDLE, 0x1234)", a, b);
        mark();
        a.ret = SetStdHandle(7, 0);
        a.err = real_err();
        a.out = "";
        mark();
        b.ret = w32_SetStdHandle(7, 0);
        b.err = stub_err();
        b.out = "";
        check("SetStdHandle(7)", a, b);
    }
    {
        // a console program: AllocConsole fails (there is one); writing to it works through the standard handle
        R a, b;
        mark();
        a.ret = AllocConsole();
        a.err = real_err();
        mark();
        b.ret = w32_AllocConsole();
        b.err = stub_err();
        check("AllocConsole (a console program)", a, b);
        HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        const uint32_t uo = w32_GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD m;
        const bool console = GetConsoleMode(ho, &m) != 0;
        DWORD n1 = 0;
        uint32_t n2 = 0;
        mark();
        a.ret = WriteConsoleA(ho, "", 0, &n1, 0);
        a.err = real_err();
        a.out = fmt("%lu", n1);
        mark();
        b.ret = w32_WriteConsoleA(uo, "", 0, &n2, 0);
        b.err = stub_err();
        b.out = fmt("%u", n2);
        check(fmt("WriteConsoleA(stdout, 0 bytes) (%s)", console ? "a console" : "redirected"), a, b);
        const HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
        const uint32_t ui = w32_GetStdHandle(STD_INPUT_HANDLE);
        DWORD old = 0;
        GetConsoleMode(hi, &old);
        mark();
        a.ret = SetConsoleMode(hi, old);
        a.err = real_err();
        a.out = "";
        mark();
        b.ret = w32_SetConsoleMode(ui, old);
        b.err = stub_err();
        b.out = "";
        check("SetConsoleMode(stdin, its mode)", a, b);
        Twin t = twin("con", [](const std::string& d) { put_file(d + "\\f", "x"); });
        HANDLE fr;
        uint32_t fs;
        create_real(t.real + "\\f", GENERIC_READ | GENERIC_WRITE, 1, OPEN_EXISTING, 0, &fr);
        create_stub(t.stub + "\\f", GENERIC_READ | GENERIC_WRITE, 1, OPEN_EXISTING, 0, &fs);
        mark();
        a.ret = WriteConsoleA(fr, "x", 1, &n1, 0);
        a.err = real_err();
        mark();
        b.ret = w32_WriteConsoleA(fs, "x", 1, &n2, 0);
        b.err = stub_err();
        check("WriteConsoleA(a disk file)", a, b);
        mark();
        a.ret = SetConsoleMode(fr, 7);
        a.err = real_err();
        mark();
        b.ret = w32_SetConsoleMode(fs, 7);
        b.err = stub_err();
        check("SetConsoleMode(a disk file, 7)", a, b);
        CloseHandle(fr);
        w32_CloseHandle(fs);
    }
    {
        R a;
        mark();
        a.ret = w32_CreateMailslotA("\\\\.\\mailslot\\mgi\\x", 0x104, 200, 0);
        a.err = stub_err();
        expect("CreateMailslotA: not available (INVALID_HANDLE_VALUE)", a.ret == 0xFFFFFFFFu && a.err == w32::ERR_NOT_SUPPORTED);
        uint32_t got = 0;
        mark();
        a.ret = w32_DeviceIoControl(0x1234, 4, 0, 0, 0, 0, &got, 0);
        a.err = stub_err();
        expect("DeviceIoControl(bad handle): ERROR_INVALID_HANDLE", !a.ret && a.err == w32::ERR_INVALID_HANDLE);
    }
    expect("error_from_errno(ENOENT)", w32::os::error_from_errno(ENOENT, false) == 2 && w32::os::error_from_errno(ENOENT, true) == 3);
    expect("error_from_errno(EEXIST, EACCES, EMFILE)", w32::os::error_from_errno(EEXIST, false) == 80 &&
                                                          w32::os::error_from_errno(EACCES, false) == 5 &&
                                                          w32::os::error_from_errno(EMFILE, false) == 4);
    expect("GetLastError / SetLastError", (w32_SetLastError(1234), w32_GetLastError() == 1234));
}

int main(int argc, char** argv) {
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    g_base = std::string(tmp) + "w32_file_test";
    if (g_base.find("\\\\") != std::string::npos) g_base.erase(g_base.find("\\\\"), 1);
    rm_tree(g_base);
    CreateDirectoryA(g_base.c_str(), 0);
    CreateDirectoryA((g_base + "\\real").c_str(), 0);
    CreateDirectoryA((g_base + "\\stub").c_str(), 0);
    printf("w32_file_test: the stand-ins against Windows, in %s\n", g_base.c_str());
    test_paths();
    test_create();
    test_sharing();
    test_io();
    test_names();
    test_find();
    test_maps();
    test_std();
    rm_tree(g_base);
    printf("%d agree, %d differ\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
