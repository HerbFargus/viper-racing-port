// w32_vpos_test.cpp -- the port's own OS calls (hook/vp_os.cpp) against real Windows (relink stage R2a.1).
//
// What vp_os.cpp implements itself, rather than handing to a stand-in, is checked here call for call against the
// Windows function it stands in for:
//   ini      GetPrivateProfileStringA / GetPrivateProfileIntA: hand-written files (sections and keys with blanks, case,
//            quotes, comments, duplicate sections and keys, CR / LF / CRLF, a BOM, missing files, '=' in values,
//            numbers in every base and sign, overflow, small buffers, defaults with blanks) and a random sweep
//            (random files from a small alphabet that hits every rule, random lookups)
//   attrs    GetFileAttributesA for what the port asks: a folder (with and without a trailing '\'), a file, a read-only
//            file, a missing file, a missing folder's file, "file\"; the result and the last error
//   events   the port's own events: auto-reset (one waiter, reset by it) and manual-reset, initial state, timeouts,
//            Wait on a closed handle, SetEvent on a non-event, CloseHandle twice -- against Windows' events
//   strings  lstrcpynA, wsprintfA (the formats the port uses)
// build (repository root), either compiler:
//   cl /nologo /O2 /MT /W3 /EHsc /std:c++17 /Ihook test\vp_os_test.cpp hook\vp_os.cpp hook\w32_handle.cpp
//      /Fo%TEMP%\vpos\ /Fe%TEMP%\vpos\w32_vpos_test.exe user32.lib
//   C:\msys64\mingw32\bin\g++ -m32 -O2 -std=c++17 -fms-extensions -Ihook test\vp_os_test.cpp hook\vp_os.cpp
//      hook\w32_handle.cpp -static -o %TEMP%\vpos\w32_vpos_test.exe
// run: w32_vpos_test.exe [random-ini-files]   -- prints each area's PASS / FAIL and the first differences; exit code =
// the number of failures. Works in its own folder under %TEMP%.
#define _CRT_SECURE_NO_WARNINGS
#include "vp_os.h"
#include "w32_handle.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static int g_fail, g_checks;
static int g_shown;
static void bad(const char* fmt, ...) {
    g_fail++;
    if (g_shown++ > 40) return;
    va_list ap;
    va_start(ap, fmt);
    printf("  DIFF: ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static char g_dir[MAX_PATH], g_ini[MAX_PATH];

static void write_file(const char* path, const std::string& s) {
    FILE* f = fopen(path, "wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static std::string show(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c < 32 || c > 126) { char b[8]; sprintf(b, "\\x%02x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// one lookup, both ways
static void ini_check(const char* sec, const char* key, const char* dflt, DWORD n, const std::string& file_text) {
    char a[300], b[300];
    memset(a, '#', sizeof a);
    memset(b, '#', sizeof b);
    const DWORD ra = GetPrivateProfileStringA(sec, key, dflt, a, n, g_ini);
    const DWORD rb = vpos::profile_string(sec, key, dflt, b, n, g_ini);
    g_checks++;
    if (ra != rb || memcmp(a, b, n ? ra + 1 : 0) != 0)
        bad("string [%s] %s (default \"%s\", %lu): Windows %lu \"%s\", vp_os %lu \"%s\" -- file \"%s\"", sec, key,
            dflt ? dflt : "(null)", (unsigned long)n, (unsigned long)ra, show(std::string(a, n ? ra : 0)).c_str(),
            (unsigned long)rb, show(std::string(b, n ? rb : 0)).c_str(), show(file_text).c_str());
    const UINT ia = GetPrivateProfileIntA(sec, key, 77, g_ini);
    const UINT ib = vpos::profile_int(sec, key, 77, g_ini);
    g_checks++;
    if (ia != ib)
        bad("int [%s] %s: Windows %d, vp_os %d -- file \"%s\"", sec, key, (int)ia, (int)ib, show(file_text).c_str());
}

static void ini_tests(int randoms) {
    const int f0 = g_fail;
    struct Case { const char* text; const char* q[12]; };
    static const Case cases[] = {
        {"[s]\r\na=1\r\n  b  =  hello world  \r\nc=\"quoted\"\r\nd='single'\r\ne=\"mis'\r\nf=1 ; comment\r\n;g=5\r\nh\r\n"
         "i=\r\nj=0x10\r\nk=-5\r\nl=  12abc\r\nm=\"\"\r\nn= \" sp \" \r\no=a=b\r\n[ t ]\r\nx=1\r\n[S]\r\na=dup\r\n[u]x\r\ny=2\r\n",
         {"s:a", "s:b", "s:c", "s:d", "s:e", "s:f", "s:;g", "s:g", "s:h", "s:i", "s:j", "s:k"}},
        {"[s]\r\nl=  12abc\r\nm=\"\"\r\nn= \" sp \" \r\no=a=b\r\n[ t ]\r\nx=1\r\n[S]\r\na=dup\r\n[u]x\r\ny=2\r\n",
         {"s:l", "s:m", "s:n", "s:o", "S:A", "s: a", "t:x", " t :x", "u:y", "u]x:y", "nosec:a", "s:zz"}},
        {"\xEF\xBB\xBF[s]\na=bom\n[t]\nb=2\n", {"s:a", "t:b"}},
        {"[s]\na=lf\n\n  [v]  \nb=2\n\t[w]\nc=3\n", {"s:a", "v:b", "w:c"}},
        {"a=nosec\n[s]\na=1\n", {"s:a"}},
        {"[s]\na=1\n[s]\nb=2\n[t]\n[s]\nc=3\n", {"s:a", "s:b", "s:c"}},
        {"[s]\na=1\na=2\nA=3\n", {"s:a"}},
        {"[s]\n\ta\t=\tv\t\n", {"s:a"}},
        {"[s]\na=1\r[t]\r\nb=2\n\r\nc=3\n\n\rd=4", {"s:a", "t:b", "t:c", "t:d"}},
        {"[s]\na=99999999999\nb=+7\nc= 4\nd=4294967295\ne=0X10\nf=-0x10\ng=010\nh=0b101\ni=0o17\nj=0x\nk=-\nl=+\n",
         {"s:a", "s:b", "s:c", "s:d", "s:e", "s:f", "s:g", "s:h", "s:i", "s:j", "s:k", "s:l"}},
        {"[s]\na=- 5\nb=0xFFFFFFFF\nc=0x1g\nd=12345678901234567890123456789012345\ne=1e3\nf=0x7fffffff\ng=2147483648\n"
         "h=-2147483649\ni=\"5\"\nj=5\"\nk=.5\nl=00x5\n",
         {"s:a", "s:b", "s:c", "s:d", "s:e", "s:f", "s:g", "s:h", "s:i", "s:j", "s:k", "s:l"}},
        {"[s]\na=--5\nb=4294967296\nc=0b2\nd=0o9\ne=0xAbC\nf=\t-\t3\ng=0b\nh=1-2\n",
         {"s:a", "s:b", "s:c", "s:d", "s:e", "s:f", "s:g", "s:h"}},
        {"[s]\n[ s2 x ]\na=1\n[s3\nc=3\n[]\nd=4\n", {"s2 x:a", "s2  x:a", "s3:c", "s3:d"}},
        {"[session]\nrecord=1                   ; record this run\nplay=20260927-193012\nlabel=original\n"
         "[platform]\nsdl=1\nrenderer=gl\naudio=sdl\n[port]\ndefault=new\nshadow_every=1\n",
         {"session:record", "session:play", "session:label", "platform:sdl", "platform:renderer", "port:default",
          "port:shadow_every", "port:shadow_per_tick", "test:two_copies", "replay:record", "debug:capture"}},
    };
    for (const Case& c : cases) {
        write_file(g_ini, c.text);
        for (const char* q : c.q) {
            if (!q) break;
            std::string s(q);
            const size_t colon = s.find(':');
            const std::string sec = s.substr(0, colon), key = s.substr(colon + 1);
            ini_check(sec.c_str(), key.c_str(), "DEF", 64, c.text);
        }
    }
    // defaults, buffers, a missing file, long values
    write_file(g_ini, "[s]\na=abcdef\nlong=" + std::string(200, '7') + "\n");
    const char* dflts[] = {"trail   ", "  lead", "", "x y ", "\t tab\t", "abcdefgh"};
    for (const char* d : dflts)
        for (DWORD n : {1u, 2u, 4u, 64u}) {
            ini_check("s", "zz", d, n, "(defaults)");
            ini_check("s", "a", d, n, "(buffers)");
        }
    ini_check("s", "long", "", 300, "(a 200-digit value)");
    DeleteFileA(g_ini);
    ini_check("s", "a", "dflt", 64, "(no file)");

    // the random sweep: files of random lines from an alphabet that hits every rule
    static const char* const bits[] = {"[", "]", "=", ";", "\"", "'", " ", "\t", "a", "A", "b", "s", "S", "1", "-", "0x",
                                       "0b", "9", "\r", "\n", "\r\n", "[s]", "[ s ]", "[t]", "a=", "b =", " =", "x"};
    const int nbits = sizeof bits / sizeof bits[0];
    static const char* const secs[] = {"s", "S", "t", " s", "x", "s ", "a"};
    static const char* const keys[] = {"a", "A", "b", " a", "x", ";", "a ", "s"};
    srand(12345);
    for (int r = 0; r < randoms; r++) {
        std::string t;
        const int len = rand() % 60;
        for (int i = 0; i < len; i++) t += bits[rand() % nbits];
        write_file(g_ini, t);
        for (const char* s : secs)
            for (const char* k : keys) ini_check(s, k, "D ", 40, t);
    }
    printf("ini:     %s (%d randomly made files)\n", g_fail == f0 ? "PASS" : "FAIL", randoms);
}

static void attr_check(const char* path) {
    SetLastError(0);
    const DWORD a = GetFileAttributesA(path);
    const DWORD ea = GetLastError();
    vpos::set_last_error(0);
    const DWORD b = vpos::file_attributes(path);
    const DWORD eb = vpos::last_error();
    g_checks++;
    // the port asks: does it exist, is it a folder (and why not, for the log)
    const bool same_kind = (a == INVALID_FILE_ATTRIBUTES) == (b == INVALID_FILE_ATTRIBUTES) &&
                           (a == INVALID_FILE_ATTRIBUTES || ((a ^ b) & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) == 0);
    if (!same_kind || (a == INVALID_FILE_ATTRIBUTES && ea != eb))
        bad("GetFileAttributesA(%s): Windows %08lx (error %lu), vp_os %08lx (error %lu)", path, (unsigned long)a,
            (unsigned long)ea, (unsigned long)b, (unsigned long)eb);
}

static void attr_tests() {
    const int f0 = g_fail;
    char p[MAX_PATH];
    sprintf(p, "%s\\sub", g_dir);
    CreateDirectoryA(p, 0);
    attr_check(p);
    sprintf(p, "%s\\sub\\", g_dir);
    attr_check(p);
    sprintf(p, "%s\\sub/", g_dir);
    attr_check(p);
    sprintf(p, "%s\\file.txt", g_dir);
    write_file(p, "x");
    attr_check(p);
    sprintf(p, "%s\\file.txt\\", g_dir);
    attr_check(p);
    sprintf(p, "%s\\FILE.TXT", g_dir);
    attr_check(p);
    sprintf(p, "%s\\ro.txt", g_dir);
    write_file(p, "x");
    SetFileAttributesA(p, FILE_ATTRIBUTE_READONLY);
    attr_check(p);
    SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
    sprintf(p, "%s\\missing.txt", g_dir);
    attr_check(p);
    sprintf(p, "%s\\nodir\\missing.txt", g_dir);
    attr_check(p);
    sprintf(p, "%s\\nodir\\deeper\\missing.txt", g_dir);
    attr_check(p);
    attr_check(g_dir);
    attr_check("C:\\");
    attr_check("C:\\Windows");
    attr_check("");
    printf("attrs:   %s\n", g_fail == f0 ? "PASS" : "FAIL");
}

// the same script on a Windows event and on vp_os's: each step's result
static void event_tests() {
    const int f0 = g_fail;
    for (int manual = 0; manual < 2; manual++)
        for (int initial = 0; initial < 2; initial++) {
            HANDLE w = CreateEventA(0, manual, initial, 0);
            HANDLE v = vpos::create_event(manual, initial, 0);
            auto step = [&](const char* what, DWORD a, DWORD b) {
                g_checks++;
                if (a != b) bad("event (manual %d, initial %d) %s: Windows %lu, vp_os %lu", manual, initial, what,
                                (unsigned long)a, (unsigned long)b);
            };
            step("wait 0", WaitForSingleObject(w, 0), vpos::wait(v, 0));
            step("wait 0 again", WaitForSingleObject(w, 0), vpos::wait(v, 0));
            step("set", SetEvent(w), vpos::set_event(v));
            step("set twice", SetEvent(w), vpos::set_event(v));
            step("wait 5", WaitForSingleObject(w, 5), vpos::wait(v, 5));
            step("wait 5 again", WaitForSingleObject(w, 5), vpos::wait(v, 5));
            // a waiter on another thread, released by a set
            std::atomic<DWORD> got_w{99}, got_v{99};
            if (!manual) {
                WaitForSingleObject(w, 0);
                vpos::wait(v, 0);
            }
            std::thread tw([&] { got_w = WaitForSingleObject(w, 2000); });
            std::thread tv([&] { got_v = vpos::wait(v, 2000); });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            SetEvent(w);
            vpos::set_event(v);
            tw.join();
            tv.join();
            step("a waiter released by a set", got_w, got_v);
            step("wait 0 after the release", WaitForSingleObject(w, 0), vpos::wait(v, 0));
            // a timeout really waits
            if (!manual) {
                const auto t0 = std::chrono::steady_clock::now();
                const DWORD r = vpos::wait(v, 20);
                const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                g_checks++;
                if (r != WAIT_TIMEOUT || ms < 15) bad("event: a 20 ms wait returned %lu after %ld ms", (unsigned long)r, ms);
            }
            step("close", CloseHandle(w), vpos::close_handle(v));
            // (the last error the port reads: without the stand-ins, as here, Windows'; with them, theirs)
            SetLastError(0);
            const DWORD wa = WaitForSingleObject(w, 0), ea = GetLastError();
            vpos::set_last_error(0);
            const DWORD wb = vpos::wait(v, 0), eb = vpos::last_error();
            step("wait on the closed handle", wa, wb);
            step("its last error", ea, eb);
            step("close again", CloseHandle(w), vpos::close_handle(v));
        }
    // two auto-reset waiters, one set: exactly one released
    {
        HANDLE v = vpos::create_event(FALSE, FALSE, 0);
        std::atomic<int> released{0};
        std::thread a([&] { if (vpos::wait(v, 200) == WAIT_OBJECT_0) released++; });
        std::thread b([&] { if (vpos::wait(v, 200) == WAIT_OBJECT_0) released++; });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        vpos::set_event(v);
        a.join();
        b.join();
        g_checks++;
        if (released != 1) bad("auto-reset event: one set released %d waiters (Windows: 1)", (int)released);
        vpos::close_handle(v);
    }
    // a named event is refused (the port never names one)
    g_checks++;
    if (vpos::create_event(FALSE, FALSE, "x")) bad("a named event was made");
    // SetEvent on a handle of the stand-ins' that isn't an event
    {
        struct Plain : w32::Object { Plain() : w32::Object(w32::Kind::Other) {} };
        const w32::Handle h = w32::add(std::make_shared<Plain>());
        vpos::set_last_error(0);
        const BOOL r = vpos::set_event((HANDLE)(uintptr_t)h);
        const DWORD e = vpos::last_error();
        g_checks++;
        if (r || e != ERROR_INVALID_HANDLE) bad("SetEvent on a non-event: %d, error %lu", r, (unsigned long)e);
        w32::close(h);
    }
    printf("events:  %s\n", g_fail == f0 ? "PASS" : "FAIL");
}

static void string_tests() {
    const int f0 = g_fail;
    const char* src[] = {"", "a", "abc", "viperport.ini", "a much longer string than the buffer"};
    for (const char* s : src)
        for (int n : {0, 1, 2, 3, 4, 8, 64}) {
            char a[80], b[80];
            memset(a, '#', sizeof a);
            memset(b, '#', sizeof b);
            lstrcpynA(a, s, n);
            vpos_lstrcpynA(b, s, n);
            g_checks++;
            if (memcmp(a, b, sizeof a)) bad("lstrcpynA(\"%s\", %d): \"%.20s\" / \"%.20s\"", s, n, a, b);
        }
    char a[64], b[64];
    const unsigned long vals[] = {0, 1, 0x4d0000, 0xffffffff, 12345};
    for (unsigned long v : vals) {
        const char* fmts[] = {"race.exe %08lx", "dinput.dll+%05lx", " %08lx", " dll+%05lx", "%lu"};
        for (const char* f : fmts) {
            const int ra = wsprintfA(a, f, v), rb = vpos_wsprintfA(b, f, v);
            g_checks++;
            if (ra != rb || strcmp(a, b)) bad("wsprintfA(%s, %lu): %d \"%s\" / %d \"%s\"", f, v, ra, a, rb, b);
        }
    }
    printf("strings: %s\n", g_fail == f0 ? "PASS" : "FAIL");
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    const int randoms = argc > 1 ? atoi(argv[1]) : 3000;
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    sprintf(g_dir, "%svp_os_test_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_dir, 0);
    sprintf(g_ini, "%s\\t.ini", g_dir);
#ifdef VP_GCC
    printf("vp_os_test (GCC)\n");
#else
    printf("vp_os_test (MSVC)\n");
#endif
    ini_tests(randoms);
    attr_tests();
    event_tests();
    string_tests();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    char cmd[MAX_PATH + 32];
    sprintf(cmd, "rmdir /s /q \"%s\"", g_dir);
    system(cmd);
    return g_fail;
}
