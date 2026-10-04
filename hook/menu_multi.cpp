// menu_multi.cpp -- multiplayer stage N3 (group A): mmulti.obj, the Multiplayer screen, rewritten faithfully (library `menu`).
//
//   MenuMultiChooseTransport (the screen: three tabs -- Lan, Direct, Modem -- chosen by radio buttons in group mode and the
//   Up / Down keys (prev_tab_cb / next_tab_cb), "Multi:Version %d", the phone number and the tab kept in the options; the
//   choice handed back in a MultiGenesisInfo, whose constructor is here too). The LAN tab is a NetBrowser: one SessionMgr
//   per protocol (CreateSocket: SocketCreateUDP / SocketCreateIPX on port 2001), broadcast for games every 4.5 s (Update,
//   which lists them, marking a newer or older version), TCP/IP or IPX (the net_protocol option), Create (new_server: a
//   name and password through MyDoInputBoxN), Find (FindHost / find_host / async_find_host: a host name, a lookup in a
//   cancel box), Connect (connect: the password when the game has one); DestroySockets waits up to 3 s for each manager
//   to finish. The line tabs are LineControls (ModemLineControl, DirectLineControl): the devices (LineEnumerateDevices) as
//   radio buttons, Call / Answer / Connect (place_call, answer: a cancel box with the device's status, connecting /
//   answering its idle functions) and then line_check (a cancel box while LineChecker pings the other end: checking,
//   update_check_msg); destroy_device waits up to 3 s for the device to shut down. MyDoCancelBox / cb_idle_func: a box with
//   a message, closed by Cancel or when its idle function says so.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (the toolkit's, the options',
// the multi library's -- now rewritten, called as the original calls it --, this file's own), virtual calls through the
// vtable. Item lists are built in frames laid out as the original's (fr.b + k is the original's [esp after its `sub esp`
// + k]), with UIDialogItem's constructor called where the original calls it and every dword stored where it stores an
// item inline; the translated texts read after their Xlator is refreshed, in the original's order. MenuMultiChooseTransport
// keeps its whole frame so: the NetBrowser, the items, the version text and the two line controls sit where the original's
// do relative to each other.
//
// No N0 patched site (net_wsock.cpp's k_grab_sites / k_release_sites / k_ptimenow_sites / k_random_sites) lies in
// mmulti.obj: its PTimeNow calls are the plain kernel ones, called by address here as the original calls them.
//
// Footprints (main thread). replay_only: everything that runs a dialog (MenuMultiChooseTransport, MyDoInputBoxN,
// MyDoCancelBox, new_server, FindHost, find_host, connect, place_call, answer, line_check and the tabs' callbacks), builds
// widgets (the Addeds, add_line_devices), adds or removes notifications (Create / Destroy), reads or writes the options (the
// constructors, the destructors), sends, receives or makes sockets (CreateSocket, DestroySockets, Tick, Update,
// reset_active_proto and NetBrowser::Callback, async_find_host), polls or shuts a line device (checking, connecting,
// answering and their static forms, destroy_device, disconnect), constructs a function-local Xlator (update_check_msg on
// its first call) or calls a function pointer it's given (cb_idle_func), frees (the deleting destructors). The tab
// callbacks write the tab; LineControl::Callback shows and hides the running window's groups; NetBrowser::Draw writes the
// canvas it's given; GetDevice, ~LineControl and MultiGenesisInfo's constructor their object.
//
// Fixes (// FIX:, docs/FIXES.md "Multiplayer menus"; VP_FIX, off in the faithful harness build): texts held to their
// buffers (MyDoInputBoxN's copies, find_host's title, NetBrowser::Update's lines, the line tabs' titles and messages, the
// version text), the line check's verdicts no longer used as a printf format when they'd read an argument, CreateSocket's
// protocol and the LAN list's selection checked. The FIX CANDIDATE left: line_check's stale checker pointer (nothing reads it).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "menu_options.h"
#include "menu_multi.h"

namespace {
namespace menu_multi {
using namespace uit;
using namespace mob;
using namespace mmu;

// a frame laid out as the original's: t(k) is the original's [esp + k] just after its `sub esp, N`
template <uint32_t N> struct Fr {
    alignas(4) uint8_t b[N];
    __forceinline void* t(uint32_t k) { return b + k; }
    __forceinline volatile uint32_t& d(uint32_t k) { return *(volatile uint32_t*)(b + k); }
};
#define G(a) UI_GU32(a)
#define S(o) ((uint32_t)(uintptr_t)self + (uint32_t)(o))
#define IC(k, ...) item_ctor(fr.t(k), __VA_ARGS__)
static __forceinline void raw(void* p, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t text,
                              uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi, uint32_t sel, uint32_t s34) {
    volatile uint32_t* d = (volatile uint32_t*)p;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = lo; d[11] = hi; d[12] = sel; d[13] = s34;
}
#define RAW(k, ...) raw(fr.t(k), __VA_ARGS__)
static __forceinline const char* sec_multi() { return UI_GP(const char, S_SEC_MULTI); }
static __forceinline int32_t now() { return ccall<int32_t>(F_PTimeNow); }
// 32-bit wrapping arithmetic on the clock (the original's `add` / `sub`)
static __forceinline int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static __forceinline int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static __forceinline uint32_t multi_version() { return G(S_MULTI_VERSION); }
// a UIDialog on the frame at k: title, background, default id, items, idle
static __forceinline void dialog(void* p, uint32_t title, uint32_t back, uint32_t def, uint32_t items, uint32_t idle) {
    volatile uint32_t* d = (volatile uint32_t*)p;
    d[0] = title; d[1] = back; d[2] = def; d[3] = items; d[4] = idle;
}

// ---- the fixes' helpers (docs/PORTING.md, "Fixes"; each use is marked // FIX:) --------------------------------------------
// A text overrunning its buffer is caught by predicting what the game's sprintf prints: such a text is formatted in a buffer
// of the rewrite's (each %s argument cut to 255 characters, so nothing passes it) and what fits is kept; every other text is
// formatted where the original formats it, so it's the original's bytes and calls.
// a string's length, counted up to 0x400 (anything that long overruns every buffer here)
static __forceinline uint32_t fx_len(const char* s) { return ui_strnlen(s, 0x400); }
// the characters the game's %d prints for v (a '-' and the digits)
static __forceinline uint32_t fx_dec(int32_t v) {
    uint32_t n = v < 0 ? 2u : 1u;
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    while (u >= 10) { u /= 10; n++; }
    return n;
}
// s, or (in buf, max + 1 bytes) its first max characters when it's longer
static __forceinline const char* fx_cut(const char* s, char* buf, uint32_t max) {
    if (ui_strnlen(s, max) <= max) return s;
    ui_copy_bounded(buf, s, max + 1);
    return buf;
}
// whether the game's printf (output.obj, 0x4d22b0), given s as its format, would read an argument: its state machine run
// over s from its own class table (.rdata 0x4e0200), as hook/menu_board.cpp's fix_format_reads_args does -- a '*' taken as
// a width or precision (states 3, 5), or reaching a conversion (state 7); 'I' in the size state skips a following "64"
static bool fx_format_reads_args(const char* s) {
    enum : uint32_t { T = 0x004e0200 };
    uint32_t state = 0;
    for (const volatile char* p = s; *p; p++) {
        const int32_t c = (int8_t)*p;
        const uint32_t cls = c < 0x20 || c > 0x78 ? 0u : (uint32_t)(UI_G8(T + (uint32_t)(c - 0x20)) & 0xf);
        state = (uint32_t)(UI_G8(T + cls * 8 + state) >> 4) & 7u;
        if ((state == 3 || state == 5) && c == '*') return true;
        if (state == 7) return true;
        if (state == 6 && c == 'I') {
            if (p[1] == '6' && p[2] == '4') p += 2;
            else state = 0;
        }
    }
    return false;
}
// the games the protocol's manager lists (SessionMgr +0x24: the count Update lists): is `sel` one of them?
static __forceinline bool fx_sel_listed(const void* mgr, int32_t sel) {
    return sel >= 0 && sel < *(const volatile int32_t*)((const uint8_t*)mgr + 0x24);
}

template <typename T> static void fp_dialog0(Footprint& f, T*, Edx) { f.replay_only = "runs a dialog (modal)"; }
template <typename T> static void fp_items0(Footprint& f, T*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates)"; }
template <typename T> static void fp_notes0(Footprint& f, T*, Edx) { f.replay_only = "adds or removes notifications (allocates or frees their copies)"; }
template <typename T> static void fp_net0(Footprint& f, T*, Edx) { f.replay_only = "ticks the session managers (sends and receives)"; }
template <typename T> static void fp_line0(Footprint& f, T*, Edx) { f.replay_only = "polls or shuts the line device (TAPI / a COM port)"; }
template <typename T> static void fp_frees(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object (and writes the options)"; }
static void fp_cb_dialog(Footprint& f, int32_t) { f.replay_only = "runs a dialog (modal)"; }
static void fp_static_line(Footprint& f) { f.replay_only = "polls the line device (TAPI / a COM port)"; }

// =====================================================================================================================
// the boxes: MyDoInputBoxN, MyDoCancelBox, cb_idle_func
// =====================================================================================================================
// two labelled text fields (their sizes capped at 32 characters' width), Ok (-2) and Cancel; on Ok both copied back
// FIX: bufs[0] / bufs[1] were copied into 0x100-byte locals unbounded, and back unbounded (the callers' are 0x20 and 0x10).
// A caller's text that doesn't end inside 255 characters overran the frame; it's cut to 255. On Ok each is copied back held
// to its size (sizes[i] bytes, when that's 1..0x100). The fields take at most sizes[i] - 1 characters, so a text that
// fitted to begin with comes back exactly as before.
static __forceinline void fx_copy_back(char* dst, const char* src, int32_t size) {
    if (VP_FIX) ui_copy_bounded(dst, src, size >= 1 && size <= 0x100 ? (uint32_t)size : 0x100u);
    else crt_strcpy(dst, src);
}
static uint8_t __cdecl MyDoInputBoxN_n(const char* title, const char* const* labels, char** bufs, int32_t* sizes, int32_t) {
    Fr<0x39c> fr;
    once_xl(0x0057a43c, 1, 0x0057a3d8, 0x004f8bc8, 0x0048cde0);
    once_xl(0x0057a43c, 2, 0x0057a510, 0x004f8bd0, 0x0048cdd0);
    char* const b0 = (char*)fr.t(0x19c);
    char* const b1 = (char*)fr.t(0x29c);
    if (VP_FIX) {                                                     // FIX: (above) cut to the locals' 0x100 bytes
        ui_copy_bounded(b0, ((char* const volatile*)bufs)[0], 0x100);
        ui_copy_bounded(b1, ((char* const volatile*)bufs)[1], 0x100);
    } else {
        crt_strcpy(b0, ((char* const volatile*)bufs)[0]);
        crt_strcpy(b1, ((char* const volatile*)bufs)[1]);
    }
    {
        const uint32_t l0 = U(((const char* const volatile*)labels)[0]);
        RAW(0x14, 5, 0, 0xa0, 0x28, 0, 0, l0, 0, 0, 0xe, 0, 0, 0, 0);
        const int32_t n0 = ((const volatile int32_t*)sizes)[0];
        const int32_t w0 = (n0 < 0x20 ? n0 : 0x20) << 3;
        RAW(0x4c, 9, 0, 0x28, 0x37, (uint32_t)w0, 0x10, 1, (uint32_t)n0, U(b0), 9, 0, 0, 0, 0);
        IC(0x84, 5, 0, 0xa0, 0x64, 0, 0, U(((const char* const volatile*)labels)[1]), 0, 0, 0xe, 0, 0, 0, 0);
        const int32_t n1 = ((const volatile int32_t*)sizes)[1];
        const int32_t w1 = (n1 < 0x20 ? n1 : 0x20) << 3;
        RAW(0xbc, 9, 0, 0x28, 0x73, (uint32_t)w1, 0x10, 1, (uint32_t)n1, U(b1), 9, 0, 0, 0, 0);
    }
    xl(0x0057a3d8);
    IC(0xf4, 2, (uint32_t)-2, 8, 0x99, 0, 0, G(0x0057a3dc), 0, 0, 0, 0, 0, 0, 0);
    xl(0x0057a510);
    RAW(0x12c, 2, (uint32_t)-1, 0xd4, 0x99, 0, 0, G(0x0057a514), 0, 0, 0, 0, 0, 0, 0);
    item_end(fr.t(0x164));
    dialog(fr.t(0), U(title), 0x004f8bdc, 0, U(fr.t(0x14)), 0);
    if (ccall<int32_t>(F_UIDoDialog, fr.t(0), (int32_t)0x140, (int32_t)0xc8, (int32_t)-999, (int32_t)-999, (int32_t)1) != -2) return 0;
    fx_copy_back(((char* const volatile*)bufs)[0], b0, ((const volatile int32_t*)sizes)[0]);   // FIX: (above) held to the sizes
    fx_copy_back(((char* const volatile*)bufs)[1], b1, ((const volatile int32_t*)sizes)[1]);
    return 1;
}
static void fp_MyDoInputBoxN(Footprint& f, const char*, const char* const*, char**, int32_t*, int32_t) { f.replay_only = "runs a dialog (modal)"; }
PORT_FN(0x0048c9f0, "MyDoInputBoxN", MyDoInputBoxN_n, fp_MyDoInputBoxN)

// a message (centred: x = 160 - its width / 2) and Cancel (-1), the idle function kept for cb_idle_func; 1 if cancelled
static int32_t __cdecl MyDoCancelBox_n(const char* title, const char* msg, uint8_t(__cdecl* idle)()) {
    Fr<0xbc> fr;
    UI_GU32(S_CB_IDLE) = U((const void*)idle);
    once_xl(0x0057a384, 1, 0x0057a3b8, 0x004f8c80, 0x0048d2a0);
    const int32_t w = ccall<int32_t>(F_gxTextWidth, msg);
    const int32_t x = 0xa0 - w / 2;
    RAW(0x14, 5, 0, (uint32_t)x, 0x32, 0, 0, U(msg), 0, 0, sty(S_COL_4FC, S_COL_40C), 0, 0, 0, 0);
    xl(0x0057a3b8);
    RAW(0x4c, 2, (uint32_t)-1, 0x6e, 0x99, 0, 0, G(0x0057a3bc), 0, 0, 0, 0, 0, 0, 0);
    item_end(fr.t(0x84));
    dialog(fr.t(0), U(title), 0x004f8c8c, (uint32_t)-2, U(fr.t(0x14)), F_cb_idle_func);
    const int32_t r = ccall<int32_t>(F_UIDoDialog, fr.t(0), (int32_t)0x140, (int32_t)0xc8, (int32_t)-999, (int32_t)-999, (int32_t)1);
    return r == -2 ? 0 : 1;
}
static void fp_MyDoCancelBox(Footprint& f, const char*, const char*, uint8_t(__cdecl*)()) { f.replay_only = "runs a dialog (modal)"; }
PORT_FN(0x0048d0e0, "MyDoCancelBox", MyDoCancelBox_n, fp_MyDoCancelBox)

static uint8_t __cdecl cb_idle_func_n(int32_t*) {
    uint8_t(__cdecl* fn)() = (uint8_t(__cdecl*)())(uintptr_t)UI_GU32(S_CB_IDLE);
    if (fn && fn()) return 1;
    return 0;
}
static void fp_cb_idle_func(Footprint& f, int32_t*) { f.replay_only = "calls the cancel box's idle function"; }
PORT_FN(0x0048d280, "cb_idle_func", cb_idle_func_n, fp_cb_idle_func)

// =====================================================================================================================
// NetBrowser: the LAN tab
// =====================================================================================================================
// Create: a name (32) and a password (16) for the new game
static uint8_t __fastcall new_server_n(NetBrowser* self, Edx) {
    Fr<0x18> fr;
    self->game_name[0] = 0;
    once_xl(0x0057a410, 1, 0x0057a4c0, 0x004f8b60, 0x0048ce10);
    once_xl(0x0057a410, 2, 0x0057a588, 0x004f8b7c, 0x0048ce00);
    once_xl(0x0057a410, 4, 0x0057a548, 0x004f8ba4, 0x0048cdf0);
    xl(0x0057a588);
    fr.d(0x10) = G(0x0057a58c);
    xl(0x0057a548);
    fr.d(0x14) = G(0x0057a54c);
    fr.d(8) = S(0x48);
    fr.d(0xc) = S(0x68);
    fr.d(0) = 0x20;
    fr.d(4) = 0x10;
    xl(0x0057a4c0);
    return ccall<uint8_t>(F_MyDoInputBoxN, G(0x0057a4c4), fr.t(0x10), fr.t(8), fr.t(0), (int32_t)2);
}
PORT_FN(0x0048c8c0, "NetBrowser::new_server", new_server_n, fp_dialog0)

// Find: "host[:port]" (port 2001 if none), looked up by NetBrowser::global
static uint8_t __cdecl FindHost_n(int32_t) {
    Fr<0x20> fr;
    once_xl(0x0057a45c, 1, 0x0057a440, 0x004f8be8, 0x0048cf40);
    once_xl(0x0057a45c, 2, 0x0057a500, 0x004f8c04, 0x0048cf30);
    char* const buf = (char*)fr.t(0);
    *(volatile char*)buf = *(const volatile char*)(uintptr_t)0x004f8c2c;
    for (int i = 1; i < 0x20; i++) ((volatile char*)buf)[i] = 0;
    int16_t port = 0x7d1;
    xl(0x0057a500);
    xl(0x0057a440);
    if (ccall<uint8_t>(F_UIDoInputBox, UI_GP(const char, 0x0057a444), UI_GP(const char, 0x0057a504), buf, (int32_t)0x20)) {
        char* c = ccall<char*>(F_strchr, (const char*)buf, (int32_t)0x3a);
        if (c) {
            *(volatile char*)c = 0;
            port = (int16_t)ccall<int32_t>(F_atoi, (const char*)(c + 1));
        }
        tcall<uint8_t>(F_find_host, UI_GP(void, S_NB_GLOBAL), (const char*)buf, port);
    }
    return 0;
}
PORT_FN(0x0048ce20, "NetBrowser::FindHost", FindHost_n, fp_cb_dialog)

// the cancel box's idle function while the lookup runs: once it has ended (the block's state 1, 4 or 5) the box closes 2.5 s
// later
static uint8_t __fastcall async_find_host_n(NetBrowser* self, Edx) {
    const int32_t t = self->timer;
    if (t == 0) {
        tcall<void>(F_NetBrowser_Tick, self);
        const int32_t st = *(const volatile int32_t*)(self->block + 4);
        if (st == 1 || (st >= 4 && st <= 5)) self->timer = wadd(now(), 0x9c4);
        return 0;
    }
    if (now() > t) {
        self->timer = 0;
        return 1;
    }
    return 0;
}
PORT_FN(0x0048cf50, "NetBrowser::async_find_host", async_find_host_n, fp_net0)

// the lookup, in a cancel box titled "Finding host (port)"
// FIX: the title ("%s %s (%d)" / "%s %s": the translation, the host -- up to 31 -- and the port) was sprintf'd into 0x40
// bytes unbounded, so a translation of about 20 characters or more ran over the frame; such a title keeps its first 63
// characters (fx_len above)
static uint8_t __fastcall find_host_n(NetBrowser* self, Edx, const char* host, int16_t port) {
    Fr<0x40> fr;
    char* const buf = (char*)fr.t(0);
    self->timer = 0;
    self->block = ccall<uint8_t*>(F_MemAlloc, (int32_t)0x438);
    once_xl(0x0057a488, 1, 0x0057a490, 0x004f8c40, 0x0048d2b0);
    if (port != 0x7d1) {
        xl(0x0057a490);
        const char* s = UI_GP(const char, 0x0057a494);
        const uint32_t p = (uint32_t)(uint16_t)port;
        if (VP_FIX && fx_len(s) + 1 + fx_len(host) + 2 + fx_dec((int32_t)p) + 1 > 0x3f) {
            char t[0x400], c1[0x100], c2[0x100];
            UI_sprintf(t, (const char*)0x004f8c58, fx_cut(s, c1, 0xff), fx_cut(host, c2, 0xff), p);
            ui_copy_bounded(buf, t, 0x40);
        } else {
            UI_sprintf(buf, (const char*)0x004f8c58, s, host, p);
        }
    } else {
        xl(0x0057a490);
        const char* s = UI_GP(const char, 0x0057a494);
        if (VP_FIX && fx_len(s) + 1 + fx_len(host) > 0x3f) {            // FIX: (above)
            char t[0x400], c1[0x100], c2[0x100];
            UI_sprintf(t, (const char*)0x004f8c64, fx_cut(s, c1, 0xff), fx_cut(host, c2, 0xff));
            ui_copy_bounded(buf, t, 0x40);
        } else {
            UI_sprintf(buf, (const char*)0x004f8c64, s, host);
        }
    }
    if (*(const volatile char*)host) {
        tcall<void>(F_SessionMgr_AsyncServiceRequestByHostname, self->mgr[0], host, port, (uint32_t)0x1001, (void*)self->block);
        const int32_t r = ccall<int32_t>(F_MyDoCancelBox, (const char*)buf, (const char*)(self->block + 0x418), F_ASyncFindHost);
        if (r == 1 && self->timer == 0) tcall<void>(F_SessionMgr_AsyncCancel, self->mgr[0], (void*)self->block);
    } else {
        UI_LogReport((const char*)0x004f8c6c);
    }
    tcall<void>(F_NetBrowser_Tick, self);
    ccall<void>(F_Delete, (void*)self->block);
    self->block = 0;
    return 0;
}
static void fp_find_host(Footprint& f, NetBrowser*, Edx, const char*, int16_t) { f.replay_only = "allocates, looks a host up, runs a dialog (modal)"; }
PORT_FN(0x0048cfa0, "NetBrowser::find_host", find_host_n, fp_find_host)

static NetBrowser* __fastcall NetBrowser_ctor_n(NetBrowser* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_CustomControl;
    self->widget = 0;
    tcall<void*>(F_UIStringList_ctor, (void*)&self->list, (int32_t)0x20, (int32_t)0x40);
    self->vtbl = (const void*)(uintptr_t)VT_NetBrowser;
    *(volatile uint8_t*)&self->password[0] = 0;
    *(volatile uint8_t*)&self->game_name[0] = 0;
    self->proto = 0;
    ccall<void>(F_OptionsGetI, sec_multi(), (const char*)0x004f8c98, (void*)&self->proto);
    if (self->proto <= -1 || self->proto >= 2) self->proto = 0;
    self->sel = 0;
    self->last_broadcast = 0;
    UI_GP(NetBrowser, S_NB_GLOBAL) = self;
    for (int32_t i = 0; i < 2; i++) {
        self->mgr[i] = 0;
        void* s = ccall<void*>(F_CreateSocket, i);
        if (s) self->mgr[i] = ccall<void*>(F_CreateSessionMgr, s, (int32_t)0, (int32_t)0x20, (int32_t)0);
    }
    return self;
}
static void fp_NetBrowser_ctor(Footprint& f, NetBrowser*, Edx) { f.replay_only = "reads the options, makes sockets and session managers"; }
PORT_FN(0x0048d2c0, "NetBrowser::NetBrowser", NetBrowser_ctor_n, fp_NetBrowser_ctor)

// the protocol's socket: SocketCreateUDP / SocketCreateIPX (the table at 0x4f8b20) on port 2001
// FIX: a protocol outside 0..1 indexed past the two-entry table and called what follows it; it makes no socket (0, as for
// a protocol that isn't there). NetBrowser's constructor clamps the option and the radio buttons set only 0 or 1.
static void* __cdecl CreateSocket_n(int32_t proto) {
    if (VP_FIX && (uint32_t)proto > 1u) return 0;
    return ((void*(__cdecl*)(int32_t))(uintptr_t)UI_GU32(S_CREATE_SOCKET + 4u * (uint32_t)proto))(0x7d1);
}
static void fp_CreateSocket(Footprint& f, int32_t) { f.replay_only = "makes a socket"; }
PORT_FN(0x0048d360, "CreateSocket", CreateSocket_n, fp_CreateSocket)

// each manager given up to 3 s (ticked, the window idled) until it can be destroyed safely, then destroyed
static void __fastcall DestroySockets_n(NetBrowser* self, Edx) {
    for (int32_t i = 0; i < 2; i++) {
        void* volatile* m = &self->mgr[i];
        if (!*m) continue;
        const int32_t end = wadd(now(), 0xbb8);
        int32_t left = wsub(end, now());
        while (left > 0) {
            if (tcall<uint8_t>(F_SessionMgr_CanDestroySafely, *m)) break;
            left = end;
            tcall<void>(F_SessionMgr_Tick, *m);
            ccall<void>(F_Win32Idle);
            left = wsub(left, now());
        }
        void* p = *m;
        if (p) {
            tcall<void>(F_SessionMgr_dtor, p);
            ccall<void>(F_Delete, p);
        }
        *m = 0;
    }
}
static void fp_DestroySockets(Footprint& f, NetBrowser*, Edx) { f.replay_only = "ticks and destroys the session managers (frees)"; }
PORT_FN(0x0048d380, "NetBrowser::DestroySockets", DestroySockets_n, fp_DestroySockets)

static void __fastcall NetBrowser_dtor_n(NetBrowser* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_NetBrowser;
    tcall<void>(F_DestroySockets, self);
    UI_GP(NetBrowser, S_NB_GLOBAL) = 0;
    const int32_t p = self->proto;
    ccall<void>(F_OptionsSetI, sec_multi(), (const char*)0x004f8ca8, p);
    tcall<void>(F_UIStringList_dtor, (void*)&self->list);
    self->vtbl = (const void*)(uintptr_t)VT_CustomControl;
}
static void fp_NetBrowser_dtor(Footprint& f, NetBrowser*, Edx) { f.replay_only = "destroys the session managers, writes the options, frees the list"; }
PORT_FN(0x0048d400, "NetBrowser::~NetBrowser", NetBrowser_dtor_n, fp_NetBrowser_dtor)

static void __fastcall NetBrowser_Tick_n(NetBrowser* self, Edx) {
    for (int32_t i = 0; i < 2; i++) {
        void* m = self->mgr[i];
        if (m) tcall<void>(F_SessionMgr_Tick, m);
    }
}
PORT_FN(0x0048d440, "NetBrowser::Tick", NetBrowser_Tick_n, fp_net0)

// each frame: the managers ticked; every 4.5 s a broadcast for games (both protocols); then the active protocol's games
// listed ("name  latency", or "name  NewerVersion / OlderVersion"), Connect shown while there are any
// FIX: each line is sprintf'd into 0x40 bytes: "%-*s" pads the name to 32 but doesn't cut it, and a service's name (32
// bytes off the wire) needn't end inside its 32 bytes; the translation after it was unbounded too. A line that would pass
// 63 characters is made with the name cut to its 32 bytes, and keeps its first 63 characters (fx_len above). Every other
// line is as before (a name running a few bytes past its field into the record shows them, as it always did).
static void __fastcall NetBrowser_Update_n(NetBrowser* self, Edx) {
    Fr<0x40> fr;
    char* const buf = (char*)fr.t(0);
    tcall<void>(F_NetBrowser_Tick, self);
    if (wsub(now(), 0x1194) > self->last_broadcast) {
        for (int32_t i = 0; i < 2; i++) {
            void* m = self->mgr[i];
            if (m) tcall<void>(F_SessionMgr_ServiceRequestBroadcast, m, (uint32_t)0x1001);
        }
        self->last_broadcast = now();
    }
    void* volatile* pm = &self->mgr[self->proto];
    if (!*pm) return;
    self->list.count = 0;
    self->list.changes = self->list.changes + 1;
    const int32_t n = *(const volatile int32_t*)((const uint8_t*)*pm + 0x24);
    const uint32_t g = self->grp_connect;
    if (n) ccall<void>(F_UIShowGroup, g);
    else ccall<void>(F_UIHideGroup, g);
    const uint8_t* rs = tcall<const uint8_t*>(F_SessionMgr_GetServiceTable, self->mgr[self->proto]);
    once_xl(0x0057a390, 1, 0x0057a478, 0x004f8cb8, 0x0048d600);
    once_xl(0x0057a390, 2, 0x0057a4f0, 0x004f8cd4, 0x0048d5f0);
    if (n <= 0) return;
    const int32_t mine = (int32_t)multi_version();
    int32_t k = n;
    do {
        const int32_t v = *(const volatile int32_t*)(rs + 0x24);
        // FIX: (above) the name's printed width: at least 32
        const uint32_t nw = VP_FIX ? (fx_len((const char*)rs) > 0x20 ? fx_len((const char*)rs) : 0x20u) : 0;
        if (mine == v) {
            const int32_t lat = *(const volatile int32_t*)(rs + 0x40);
            if (VP_FIX && nw + 1 + fx_dec(lat) > 0x3f) {
                char t[0x400], c1[0x21];
                UI_sprintf(t, (const char*)0x004f8cf8, (int32_t)0x20, fx_cut((const char*)rs, c1, 0x20), lat);
                ui_copy_bounded(buf, t, 0x40);
            } else {
                UI_sprintf(buf, (const char*)0x004f8cf8, (int32_t)0x20, (const char*)rs, lat);
            }
        } else {
            const char* s;
            if (mine > v) { xl(0x0057a4f0); s = UI_GP(const char, 0x0057a4f4); }
            else { xl(0x0057a478); s = UI_GP(const char, 0x0057a47c); }
            if (VP_FIX && nw + 1 + fx_len(s) > 0x3f) {
                char t[0x400], c1[0x21], c2[0x100];
                UI_sprintf(t, (const char*)0x004f8cf0, (int32_t)0x20, fx_cut((const char*)rs, c1, 0x20), fx_cut(s, c2, 0xff));
                ui_copy_bounded(buf, t, 0x40);
            } else {
                UI_sprintf(buf, (const char*)0x004f8cf0, (int32_t)0x20, (const char*)rs, s);
            }
        }
        rs += 0x54;
        tcall<void>(F_UIStringList_AddEntry, (void*)&self->list, (const char*)buf);
    } while (--k);
}
PORT_FN(0x0048d460, "NetBrowser::Update", NetBrowser_Update_n, fp_net0)

// the protocol changed (or the tab opened): the list emptied; with a manager, Find shown for TCP/IP only and the list
// updated (Update); without, "protocol unavailable" listed and Connect hidden
static void __fastcall reset_active_proto_n(NetBrowser* self, Edx) {
    const int32_t p = self->proto;
    self->list.changes = self->list.changes + 1;
    self->list.count = 0;
    if (self->mgr[p]) {
        const uint32_t g = self->grp_find;
        if (p == 0) ccall<void>(F_UIShowGroup, g);
        else ccall<void>(F_UIHideGroup, g);
        vcall<void>(self, 0x14);                                      // Update
        return;
    }
    once_xl(0x0057a5d4, 1, 0x0057a398, 0x004f8d00, 0x0048d6b0);
    xl(0x0057a398);
    tcall<void>(F_UIStringList_AddEntry, (void*)&self->list, UI_GP(const char, 0x0057a39c));
    ccall<void>(F_UIHideGroup, self->grp_connect);
}
PORT_FN(0x0048d610, "NetBrowser::reset_active_proto", reset_active_proto_n, fp_net0)

static void __fastcall NetBrowser_Callback_n(NetBrowser* self, Edx, int32_t, const void*) { tcall<void>(F_reset_active_proto, self); }
static void fp_NetBrowser_Callback(Footprint& f, NetBrowser*, Edx, int32_t, const void*) { f.replay_only = "ticks the session managers (sends and receives)"; }
PORT_FN(0x0048d6c0, "NetBrowser::Callback", NetBrowser_Callback_n, fp_NetBrowser_Callback)

static void __fastcall NetBrowser_Create_n(NetBrowser* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->proto, (uint32_t)4);
    tcall<void>(F_reset_active_proto, self);
    ccall<void>(F_UIHideGroup, self->grp_connect);
}
PORT_FN(0x0048d6d0, "NetBrowser::Create", NetBrowser_Create_n, fp_notes0)

static void __fastcall NetBrowser_Destroy_n(NetBrowser* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->proto);
}
PORT_FN(0x0048d700, "NetBrowser::Destroy", NetBrowser_Destroy_n, fp_notes0)

// Connect: the password asked for when the chosen game has one; 1 (go) otherwise
// FIX: the list's selection wasn't checked against the games found (here and where MenuMultiChooseTransport copies the
// chosen service): a game that drops out of the list between frames leaves `sel` past the last one (or the list's -1), and
// a stale or out-of-table entry was read -- and joined. A selection that isn't one of the games listed now does nothing
// (Connect stays on the screen). (The manager is there whenever Connect is: reset_active_proto hides it otherwise.)
static uint8_t __fastcall connect_n(NetBrowser* self, Edx) {
    if (VP_FIX) {
        const void* m = self->mgr[self->proto];
        if (m && !fx_sel_listed(m, self->sel)) return 0;
    }
    const uint8_t* rs = tcall<const uint8_t*>(F_SessionMgr_GetServiceTable, self->mgr[self->proto]);
    if (!*(const volatile uint8_t*)(rs + (uint32_t)self->sel * 0x54u + 0x2c)) return 1;
    once_xl(0x0057a3f4, 1, 0x0057a5b8, 0x004f8d28, 0x0048d7f0);
    once_xl(0x0057a3f4, 2, 0x0057a308, 0x004f8d48, 0x0048d7e0);
    xl(0x0057a308);
    xl(0x0057a5b8);
    return ccall<uint8_t>(F_UIDoInputBox, UI_GP(const char, 0x0057a5bc), UI_GP(const char, 0x0057a30c), (char*)&self->password[0],
                          (int32_t)0x10);
}
PORT_FN(0x0048d710, "NetBrowser::connect", connect_n, fp_dialog0)

// TCP/IP and IPX (radio buttons on the protocol), Create, Find (in the Find group), Connect and its Enter key (in the Connect
// group), the list's title and the list
static void __fastcall NetBrowser_Added_n(NetBrowser* self, Edx) {
    Fr<0x2d8> fr;
    once_xl(0x0057a340, 1, 0x0057a5c8, 0x004f8d68, 0x0048dbf0);
    once_xl(0x0057a340, 2, 0x0057a3c8, 0x004f8d80, 0x0048dbe0);
    once_xl(0x0057a340, 4, 0x0057a360, 0x004f8d98, 0x0048dbd0);
    once_xl(0x0057a340, 8, 0x0057a5d8, 0x004f8db0, 0x0048dbc0);
    IC(0, 0xc, 0, 0x3c, 0x96, 0, 0, 0x004f8dd0, 0, S(0x2c), 8, 0, 0, 0, 0);
    IC(0x38, 0xc, 0, 0x3c, 0xaf, 0, 0, 0x004f8dd8, 1, S(0x2c), 8, 0, 0, 0, 0);
    xl(0x0057a5c8);
    IC(0x70, 2, 1, 0x28, 0xf5, 0, 0, G(0x0057a5cc), 0, F_NewServer, 0, 0, 0, 0, 0);
    IC(0xa8, 1, 0, 0, 0, 0, 0, 0, 0, S(0x40), 0, 0, 0, 0, 0);
    xl(0x0057a3c8);
    IC(0xe0, 2, 0, 0x28, 0x145, 0, 0, G(0x0057a3cc), 0, F_FindHost, 0, 0, 0, 0, 0);
    IC(0x118, 1, 1, 0, 0, 0, 0, 0, 0, S(0x40), 0, 0, 0, 0, 0);
    IC(0x150, 1, 0, 0, 0, 0, 0, 0, 0, S(0x44), 0, 0, 0, 0, 0);
    xl(0x0057a360);
    IC(0x188, 2, 0, 0x28, 0x11d, 0, 0, G(0x0057a364), 0, F_NB_Connect, 0, 0, 0, 0, 0);
    IC(0x1c0, 4, 0, 0xd, 0, 0, 0, 0, 0, F_NB_Connect, 0, 0, 0, 0, 0);
    IC(0x1f8, 1, 1, 0, 0, 0, 0, 0, 0, S(0x44), 0, 0, 0, 0, 0);
    xl(0x0057a5d8);
    RAW(0x230, 5, 0, 0xf0, 0x84, 0, 0, G(0x0057a5dc), 0, 0, sty(S_COL_4FC, S_COL_40C), 0, 0, 0, 0);
    IC(0x268, 0x14, 0, 0xf0, 0x96, 0x100, 0xc8, 0, 0, S(0x18), 0, 0, 0, S(0x3c), 0);
    item_end(fr.t(0x2a0));
    add_items(self, fr.t(0));
}
PORT_FN(0x0048d800, "NetBrowser::Added", NetBrowser_Added_n, fp_items0)

// the list's frame
static void __fastcall NetBrowser_Draw_n(NetBrowser*, Edx, gxCanvas* c) {
    const uint32_t col = G(S_COL_40C);
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    ccall<void>(F_gxLine, (int32_t)0xf0, (int32_t)0x96, (int32_t)0x1ef, (int32_t)0x96, col);
    ccall<void>(F_gxLine, (int32_t)0xf0, (int32_t)0x96, (int32_t)0xf0, (int32_t)0x15d, col);
    ccall<void>(F_gxLine, (int32_t)0x1ef, (int32_t)0x96, (int32_t)0x1ef, (int32_t)0x15e, col);
    ccall<void>(F_gxLine, (int32_t)0xf0, (int32_t)0x15d, (int32_t)0x1ef, (int32_t)0x15d, col);
}
static void fp_NetBrowser_Draw(Footprint& f, NetBrowser*, Edx, gxCanvas* c) { mo_fp_draw(f, c); }
PORT_FN(0x0048dc00, "NetBrowser::Draw", NetBrowser_Draw_n, fp_NetBrowser_Draw)

// the buttons' callbacks and the cancel box's idle function: NetBrowser::global's methods
static uint8_t __cdecl ASyncFindHost_n() { return tcall<uint8_t>(F_async_find_host, UI_GP(void, S_NB_GLOBAL)); }
static void fp_ASyncFindHost(Footprint& f) { f.replay_only = "ticks the session managers (sends and receives)"; }
PORT_FN(0x0048fa10, "NetBrowser::ASyncFindHost", ASyncFindHost_n, fp_ASyncFindHost)

static uint8_t __cdecl NewServer_n(int32_t) { return tcall<uint8_t>(F_new_server, UI_GP(void, S_NB_GLOBAL)); }
PORT_FN(0x0048fa40, "NetBrowser::NewServer", NewServer_n, fp_cb_dialog)

static uint8_t __cdecl NB_Connect_n(int32_t) { return tcall<uint8_t>(F_connect, UI_GP(void, S_NB_GLOBAL)); }
PORT_FN(0x0048fa50, "NetBrowser::Connect", NB_Connect_n, fp_cb_dialog)

// the scalar deleting destructor
static void* __fastcall NetBrowser_sdd_n(NetBrowser* self, Edx, uint32_t flags) {
    tcall<void>(F_NetBrowser_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048fa20, "NetBrowser::scalar deleting destructor", NetBrowser_sdd_n, fp_frees)

// =====================================================================================================================
// LineControl: the Direct and Modem tabs
// =====================================================================================================================
// the cancel boxes' idle functions: LineControl::global's (set by place_call / answer)
static uint8_t __cdecl LC_Checking_n() { return tcall<uint8_t>(F_LC_checking, UI_GP(void, S_LC_GLOBAL)); }
PORT_FN(0x0048dc90, "LineControl::Checking", LC_Checking_n, fp_static_line)
static uint8_t __cdecl LC_Connecting_n() { return tcall<uint8_t>(F_LC_connecting, UI_GP(void, S_LC_GLOBAL)); }
PORT_FN(0x0048dca0, "LineControl::Connecting", LC_Connecting_n, fp_static_line)
static uint8_t __cdecl LC_Answering_n() { return tcall<uint8_t>(F_LC_answering, UI_GP(void, S_LC_GLOBAL)); }
PORT_FN(0x0048dcb0, "LineControl::Answering", LC_Answering_n, fp_static_line)

// the buttons: the call made (or answered), then the line checked; the device shut if the check fails
static uint8_t __cdecl DLC_Connect_n(int32_t) {
    if (tcall<uint8_t>(F_LC_place_call, UI_GP(void, S_DLC_GLOBAL))) {
        if (tcall<uint8_t>(F_LC_line_check, UI_GP(void, S_DLC_GLOBAL))) return 1;
        tcall<void>(F_LC_disconnect, UI_GP(void, S_DLC_GLOBAL));
    }
    return 0;
}
PORT_FN(0x0048dcc0, "DirectLineControl::Connect", DLC_Connect_n, fp_cb_dialog)

static uint8_t __cdecl MLC_Answer_n(int32_t) {
    if (tcall<uint8_t>(F_LC_answer, UI_GP(void, S_MLC_GLOBAL))) {
        if (tcall<uint8_t>(F_LC_line_check, UI_GP(void, S_MLC_GLOBAL))) return 1;
        tcall<void>(F_LC_disconnect, UI_GP(void, S_MLC_GLOBAL));
    }
    return 0;
}
PORT_FN(0x0048dcf0, "ModemLineControl::Answer", MLC_Answer_n, fp_cb_dialog)

static uint8_t __cdecl MLC_Call_n(int32_t) {
    if (tcall<uint8_t>(F_LC_place_call, UI_GP(void, S_MLC_GLOBAL))) {
        if (tcall<uint8_t>(F_LC_line_check, UI_GP(void, S_MLC_GLOBAL))) return 1;
        tcall<void>(F_LC_disconnect, UI_GP(void, S_MLC_GLOBAL));
    }
    return 0;
}
PORT_FN(0x0048dd20, "ModemLineControl::Call", MLC_Call_n, fp_cb_dialog)

// kind 1: the modems (TAPI), 2: the COM ports
static LineControl* __fastcall LineControl_ctor_n(LineControl* self, Edx, uint8_t kind) {
    self->vtbl = (const void*)(uintptr_t)VT_CustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_LineControl;
    self->kind = kind;
    *(volatile char*)&self->phone[0] = 0;
    self->sel = 0;
    self->count = ccall<int32_t>(F_LineEnumerateDevices, (void*)&self->dev[0], (int32_t)5, (uint32_t)kind);
    self->device = 0;
    self->timer = 0;
    self->msg = 0;
    return self;
}
static void fp_LineControl_ctor(Footprint& f, LineControl*, Edx, uint8_t) { f.replay_only = "enumerates the line devices (TAPI, the COM ports)"; }
PORT_FN(0x0048dd50, "LineControl::LineControl", LineControl_ctor_n, fp_LineControl_ctor)

static void __fastcall LineControl_dtor_n(LineControl* self, Edx) { self->vtbl = (const void*)(uintptr_t)VT_CustomControl; }
static void fp_LineControl_dtor(Footprint& f, LineControl* self, Edx) { f.add(self, 4, "its vtable"); }
PORT_FN(0x0048ddb0, "LineControl::~LineControl", LineControl_dtor_n, fp_LineControl_dtor)

static void __fastcall LC_disconnect_n(LineControl* self, Edx) { tcall<void>(F_LC_destroy_device, self); }
PORT_FN(0x0048ddc0, "LineControl::disconnect", LC_disconnect_n, fp_line0)

// the device handed over (MenuMultiChooseTransport makes the socket on it): no longer this control's
static void* __fastcall LC_GetDevice_n(LineControl* self, Edx) {
    void* d = self->device;
    self->device = 0;
    return d;
}
static void fp_LC_GetDevice(Footprint& f, LineControl* self, Edx) { f.add((void*)&self->device, 4, "its device"); }
PORT_FN(0x0048ddd0, "LineControl::GetDevice", LC_GetDevice_n, fp_LC_GetDevice)

// a cancel box while the two ends ping each other (checking); 1 if the line is up and good (and who is the server known)
// FIX CANDIDATE: `checker` is left aimed at this frame's LineChecker. Not fixed: only checking / update_check_msg read it,
// and only from inside this box (LineControl::Checking is no other box's idle function), so nothing reads it afterwards;
// clearing it would change only a dead field
static uint8_t __fastcall LC_line_check_n(LineControl* self, Edx) {
    Fr<0x68> fr;
    uint8_t ok = 0;
    self->msg = (char*)fr.t(0x28);
    self->is_server = 0;
    tcall<void*>(F_LineChecker_ctor, fr.t(0), self->device);
    self->checker = fr.t(0);
    tcall<void>(F_LC_update_check_msg, self);
    self->timer = 0;
    once_xl(0x0057a458, 1, 0x0057a530, 0x004f8ddc, 0x0048ded0);
    xl(0x0057a530);
    if (ccall<int32_t>(F_MyDoCancelBox, UI_GP(const char, 0x0057a534), (const char*)self->msg, F_LC_Checking) != 1) {
        if (vcall<int32_t>(self->device, 0x10) == 2 && tcall<uint8_t>(F_LineChecker_LineOk, fr.t(0))) {
            ok = 1;
            self->is_server = tcall<uint8_t>(F_LineChecker_IsServer, fr.t(0));
        }
    }
    self->msg = 0;
    return ok;
}
PORT_FN(0x0048ddf0, "LineControl::line_check", LC_line_check_n, fp_dialog0)

// the device shut down (given up to 3 s, the window idled, to say it's safe), deleted
static void __fastcall LC_destroy_device_n(LineControl* self, Edx) {
    vcall<void>(self->device, 0);                                     // Shutdown
    const int32_t end = wadd(now(), 0xbb8);
    for (;;) {
        int32_t left = end;
        ccall<void>(F_Win32Idle);
        left = wsub(left, now());
        if (left <= 0) break;
        if (vcall<uint8_t>(self->device, 4)) break;                   // CanDestroySafely
    }
    void* d = self->device;
    if (d) vcall<void*>(d, 8, (uint32_t)1);                           // the deleting destructor
    self->device = 0;
}
PORT_FN(0x0048dee0, "LineControl::destroy_device", LC_destroy_device_n, fp_line0)

// FIX helper: a cancel box's message (place_call's, answer's, line_check's: each 0x40 bytes of its frame) set to a text
// (the device's status, a translation: LineStatusText), held to its 0x40 bytes -- it was copied unbounded. A text that fits
// is copied as before.
static __forceinline void fx_status(char* msg, const char* s) {
    if (VP_FIX) ui_copy_bounded(msg, s, 0x40);
    else crt_strcpy(msg, s);
}

// the idle functions of answer's and place_call's boxes: the device's status shown; once it's connected (or, answering, a
// call offered and answered) the box closes 2.5 s later
static uint8_t __fastcall LC_answering_n(LineControl* self, Edx) {
    const int32_t t = self->timer;
    if (t == 0) {
        const int32_t st = vcall<int32_t>(self->device, 0x10);
        fx_status(self->msg, ccall<const char*>(F_LineStatusText, st));  // FIX: (fx_status)
        if (st == 2) {
            self->timer = wadd(now(), 0x9c4);
            return 0;
        }
        if (st == 0xa) vcall<void>(self->device, 0x2c);               // Answer
        return 0;
    }
    if (now() > t) {
        self->timer = 0;
        return 1;
    }
    return 0;
}
PORT_FN(0x0048df40, "LineControl::answering", LC_answering_n, fp_line0)

static uint8_t __fastcall LC_connecting_n(LineControl* self, Edx) {
    const int32_t t = self->timer;
    if (t == 0) {
        const int32_t st = vcall<int32_t>(self->device, 0x10);
        fx_status(self->msg, ccall<const char*>(F_LineStatusText, st));  // FIX: (fx_status)
        if (st >= 1 && (st <= 2 || st == 9)) self->timer = wadd(now(), 0x9c4);
        return 0;
    }
    if (now() > t) {
        self->timer = 0;
        return 1;
    }
    return 0;
}
PORT_FN(0x0048dfe0, "LineControl::connecting", LC_connecting_n, fp_line0)

// "line is ok" / "line is not ok" by the pings gone unanswered (sent - got - in sequence: 4 or more is bad)
static void __fastcall LC_update_check_msg_n(LineControl* self, Edx) {
    const uint8_t* c = (const uint8_t*)self->checker;
    const int32_t bad = *(const volatile int32_t*)(c + 0x14) - *(const volatile int32_t*)(c + 0x10) - *(const volatile int32_t*)(c + 0x18);
    once_xl(0x0057a568, 1, 0x0057a418, 0x004f8e00, 0x0048e140);
    once_xl(0x0057a568, 2, 0x0057a3e8, 0x004f8e20, 0x0048e130);
    const char* s;
    if (bad >= 4) { xl(0x0057a3e8); s = UI_GP(const char, 0x0057a3ec); }
    else { xl(0x0057a418); s = UI_GP(const char, 0x0057a41c); }
    // FIX: the verdict ("%s", a translation) went into line_check's 0x40-byte message unbounded; a longer one keeps its
    // first 63 characters
    if (VP_FIX && fx_len(s) > 0x3f) ui_copy_bounded((char*)self->msg, s, 0x40);
    else UI_sprintf((char*)self->msg, (const char*)0x004f8e44, s);
}
static void fp_LC_update_check_msg(Footprint& f, LineControl*, Edx) { f.replay_only = "writes the cancel box's message (line_check's frame), constructs its Xlators"; }
PORT_FN(0x0048e070, "LineControl::update_check_msg", LC_update_check_msg_n, fp_LC_update_check_msg)

// line_check's idle function: while connected the checker runs, and when it's done the verdict is shown and the box closes
// 2.5 s later; a lost connection is shown likewise
// FIX: the verdicts are translations sprintf'd as the format itself, with nothing else passed, into line_check's 0x40-byte
// message: a '%' conversion or '*' in one read arguments from the stack (garbage, or a crash for "%s" / "%n"), and a long
// one ran over the frame. Such a text (one the game's printf would read an argument for, fx_format_reads_args, or one over
// 63 characters) is shown as written, cut to 63 characters. Every other text is the format as before ("%%" prints '%').
static __forceinline void fx_verdict(char* msg, const char* s) {
    if (VP_FIX && (fx_len(s) > 0x3f || fx_format_reads_args(s))) ui_copy_bounded(msg, s, 0x40);
    else UI_sprintf(msg, s);
}
static uint8_t __fastcall LC_checking_n(LineControl* self, Edx) {
    const int32_t t = now();
    const int32_t timer = self->timer;
    if (timer != 0) return t > timer ? 1 : 0;
    tcall<void>(F_LC_update_check_msg, self);
    if (vcall<int32_t>(self->device, 0x10) == 2) {
        tcall<void>(F_LineChecker_Tick, self->checker);
        if (!tcall<uint8_t>(F_LineChecker_DoneChecking, self->checker)) return 0;
        once_xl(0x0057a584, 1, 0x0057a4e0, 0x004f8e48, 0x0048e300);
        once_xl(0x0057a584, 2, 0x0057a3a8, 0x004f8e74, 0x0048e2f0);
        const char* s;
        if (tcall<uint8_t>(F_LineChecker_LineOk, self->checker)) { xl(0x0057a4e0); s = UI_GP(const char, 0x0057a4e4); }
        else { xl(0x0057a3a8); s = UI_GP(const char, 0x0057a3ac); }
        fx_verdict((char*)self->msg, s);                              // FIX: (fx_verdict)
        self->timer = wadd(t, 0x9c4);
        return 0;
    }
    once_xl(0x0057a584, 4, 0x0057a320, 0x004f8ea4, 0x0048e2e0);
    xl(0x0057a320);
    const char* s = UI_GP(const char, 0x0057a324);
    fx_verdict((char*)self->msg, s);                                  // FIX: (fx_verdict)
    self->timer = wadd(t, 0x9c4);
    return 0;
}
PORT_FN(0x0048e150, "LineControl::checking", LC_checking_n, fp_line0)

// Call / Connect: the chosen device made and told to call (the phone number); a cancel box ("Connecting to <number>..." /
// "Connecting...") with its status until it's connected; the device shut if not. No device: "can't open" and its error
// FIX: the title ("%s %s...": a translation and the number, up to 31; "%s...", "%s": a translation) was sprintf'd into 0x40
// bytes unbounded, and the device's status copied into the message's 0x40 unbounded (fx_status); a longer one keeps its
// first 63 characters (fx_len above). Every title that fits is formatted as before.
static void fx_title(char* title, uint32_t fmt, const char* s, const char* phone) {
    const uint32_t n = fmt == 0x004f8eecu ? fx_len(s) + 1 + fx_len(phone) + 3 : fmt == 0x004f8ef8u ? fx_len(s) + 3 : fx_len(s);
    if (VP_FIX && n > 0x3f) {
        char t[0x400], c1[0x100], c2[0x100];
        if (fmt == 0x004f8eecu) UI_sprintf(t, (const char*)(uintptr_t)fmt, fx_cut(s, c1, 0xff), fx_cut(phone, c2, 0xff));
        else UI_sprintf(t, (const char*)(uintptr_t)fmt, fx_cut(s, c1, 0xff));
        ui_copy_bounded(title, t, 0x40);
    } else if (fmt == 0x004f8eecu) {
        UI_sprintf(title, (const char*)(uintptr_t)fmt, s, phone);
    } else {
        UI_sprintf(title, (const char*)(uintptr_t)fmt, s);
    }
}
static uint8_t __fastcall LC_place_call_n(LineControl* self, Edx) {
    Fr<0x80> fr;
    char* const title = (char*)fr.t(0);
    char* const msg = (char*)fr.t(0x40);
    UI_GP(LineControl, S_LC_GLOBAL) = self;
    uint8_t ok = 0;
    LineDevInfo* info = &self->dev[self->sel];
    self->device = info->create(info);
    if (self->device) {
        once_xl(0x0057a32c, 1, 0x0057a520, 0x004f8ebc, 0x0048e550);
        once_xl(0x0057a32c, 2, 0x0057a598, 0x004f8ed4, 0x0048e540);
        if (info->flag0) {
            xl(0x0057a520);
            fx_title(title, 0x004f8eec, UI_GP(const char, 0x0057a524), (const char*)&self->phone[0]);   // FIX: (fx_title)
        } else {
            xl(0x0057a598);
            fx_title(title, 0x004f8ef8, UI_GP(const char, 0x0057a59c), 0);                             // FIX: (fx_title)
        }
        vcall<void>(self->device, 0x24, (const char*)&self->phone[0]);  // Call
        self->msg = msg;
        fx_status(msg, ccall<const char*>(F_LineStatusText, vcall<int32_t>(self->device, 0x10)));   // FIX: (fx_status)
        self->timer = 0;
        if (ccall<int32_t>(F_MyDoCancelBox, (const char*)title, (const char*)msg, F_LC_Connecting) != 1 &&
            vcall<int32_t>(self->device, 0x10) == 2)
            ok = 1;
        self->msg = 0;
        if (!ok) tcall<void>(F_LC_destroy_device, self);
    } else {
        once_xl(0x0057a32c, 4, 0x0057a378, 0x004f8f00, 0x0048e530);
        xl(0x0057a378);
        fx_title(title, 0x004f8f20, UI_GP(const char, 0x0057a37c), 0);                                 // FIX: (fx_title)
        const char* e = tcall<const char*>(F_LineDeviceInfo_GetError, info);
        ccall<void>(F_UIDoOkBox, (const char*)title, e);
    }
    return ok;
}
PORT_FN(0x0048e310, "LineControl::place_call", LC_place_call_n, fp_dialog0)

// Answer: the chosen device made; a cancel box ("Waiting for a call") with its status until it's connected
// FIX: as place_call's: the title (a translation) and the status went into 0x40 bytes each unbounded; each keeps 63
static uint8_t __fastcall LC_answer_n(LineControl* self, Edx) {
    Fr<0x80> fr;
    char* const title = (char*)fr.t(0);
    char* const msg = (char*)fr.t(0x40);
    UI_GP(LineControl, S_LC_GLOBAL) = self;
    uint8_t ok = 0;
    LineDevInfo* info = &self->dev[self->sel];
    void* d = info->create(info);
    self->device = d;
    if (d) {
        self->msg = msg;
        fx_status(msg, ccall<const char*>(F_LineStatusText, vcall<int32_t>(d, 0x10)));             // FIX: (fx_status)
        self->timer = 0;
        once_xl(0x0057a318, 1, 0x0057a460, 0x004f8f24, 0x0048e720);
        xl(0x0057a460);
        fx_status(title, UI_GP(const char, 0x0057a464));               // FIX: (fx_status: the title's 0x40 bytes too)
        if (ccall<int32_t>(F_MyDoCancelBox, (const char*)title, (const char*)msg, F_LC_Answering) != 1 &&
            vcall<int32_t>(self->device, 0x10) == 2)
            ok = 1;
        self->msg = 0;
        if (!ok) tcall<void>(F_LC_destroy_device, self);
    } else {
        once_xl(0x0057a318, 2, 0x0057a5a8, 0x004f8f44, 0x0048e710);
        xl(0x0057a5a8);
        fx_title(title, 0x004f8f64, UI_GP(const char, 0x0057a5ac), 0);                                 // FIX: (fx_title)
        const char* e = tcall<const char*>(F_LineDeviceInfo_GetError, info);
        ccall<void>(F_UIDoOkBox, (const char*)title, e);
    }
    return ok;
}
PORT_FN(0x0048e560, "LineControl::answer", LC_answer_n, fp_dialog0)

// the device chosen: "in use" shown (the controls hidden) if it's in use, else the controls; no devices: that message alone
static void __fastcall LineControl_Callback_n(LineControl* self, Edx, int32_t, const void*) {
    if (self->count != 0) {
        const uint8_t in_use = self->dev[self->sel].in_use;
        const uint32_t g = self->grp_in_use;
        if (in_use) {
            ccall<void>(F_UIShowGroup, g);
            ccall<void>(F_UIHideGroup, self->grp_controls);
        } else {
            ccall<void>(F_UIHideGroup, g);
            ccall<void>(F_UIShowGroup, self->grp_controls);
        }
        ccall<void>(F_UIHideGroup, self->grp_none);
        return;
    }
    ccall<void>(F_UIHideGroup, self->grp_in_use);
    ccall<void>(F_UIHideGroup, self->grp_controls);
    ccall<void>(F_UIShowGroup, self->grp_none);
}
static void fp_LineControl_Callback(Footprint& f, LineControl*, Edx, int32_t, const void*) {
    ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
}
PORT_FN(0x0048e730, "LineControl::Callback", LineControl_Callback_n, fp_LineControl_Callback)

static void __fastcall LineControl_Create_n(LineControl* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->sel, (uint32_t)4);
    vcall<void>(self, 0x10, (int32_t)0, (const void*)&self->sel);    // Callback(0, &sel)
}
PORT_FN(0x0048e7b0, "LineControl::Create", LineControl_Create_n, fp_notes0)

static void __fastcall LineControl_Destroy_n(LineControl* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->sel);
}
PORT_FN(0x0048e7d0, "LineControl::Destroy", LineControl_Destroy_n, fp_notes0)

// one radio button per device (its name), 0x1e apart from y 0x8c
static void __fastcall LC_add_line_devices_n(LineControl* self, Edx) {
    Fr<0x7c> fr;
    fr.d(0) = U(self);
    if (self->count <= 0) return;
    uint32_t name = S(0x52);
    fr.d(4) = 0x8c;
    fr.d(8) = S(0x48);
    int32_t i = 0;
    do {
        RAW(0xc, 0xc, 0, 0x96, fr.d(4), 0, 0, name, (uint32_t)i, fr.d(8), 8, 0, 0, 0, 0);
        item_end(fr.t(0x44));
        name += 0x50;
        i++;
        add_items((void*)(uintptr_t)fr.d(0), fr.t(0xc));
        fr.d(4) = fr.d(4) + 0x1e;
    } while (((LineControl*)(uintptr_t)fr.d(0))->count > i);
}
PORT_FN(0x0048e7e0, "LineControl::add_line_devices", LC_add_line_devices_n, fp_items0)

// the "in use" and "no devices" messages, each in its group; then the devices
static void __fastcall LineControl_Added_n(LineControl* self, Edx) {
    Fr<0x188> fr;
    once_xl(0x0057a564, 1, 0x0057a330, 0x004f8f68, 0x0048eb80);
    once_xl(0x0057a564, 2, 0x0057a578, 0x004f8f9c, 0x0048eb70);
    RAW(0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x40), 0, 0, 0, 0, 0);
    xl(0x0057a330);
    IC(0x38, 5, 0, 0x32, 0x127, 0, 0, G(0x0057a334), 0, 0, sty(S_COL_4FC, S_COL_56C), 0, 0, 0, 0);
    RAW(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0x40), 0, 0, 0, 0, 0);
    IC(0xa8, 1, 0, 0, 0, 0, 0, 0, 0, S(0x3c), 0, 0, 0, 0, 0);
    xl(0x0057a578);
    RAW(0xe0, 5, 0, 0x32, 0xc3, 0, 0, G(0x0057a57c), 0, 0, sty(S_COL_4FC, S_COL_56C), 0, 0, 0, 0);
    RAW(0x118, 1, 1, 0, 0, 0, 0, 0, 0, S(0x3c), 0, 0, 0, 0, 0);
    item_end(fr.t(0x150));
    add_items(self, fr.t(0));
    tcall<void>(F_LC_add_line_devices, self);
}
PORT_FN(0x0048e8b0, "LineControl::Added", LineControl_Added_n, fp_items0)

// Call, the number (32), Answer -- in the controls' group; then LineControl's
static void __fastcall ModemLineControl_Added_n(LineControl* self, Edx) {
    Fr<0x188> fr;
    once_xl(0x0057a454, 1, 0x0057a2f8, 0x004f8fc4, 0x0048eec0);
    once_xl(0x0057a454, 2, 0x0057a400, 0x004f8fd4, 0x0048eeb0);
    once_xl(0x0057a454, 4, 0x0057a558, 0x004f8fec, 0x0048eea0);
    RAW(0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x18), 0, 0, 0, 0, 0);
    xl(0x0057a2f8);
    IC(0x38, 2, 0, 0x28, 0x11d, 0, 0, G(0x0057a2fc), 0, F_MLC_Call, 0, 0, 0, 0, 0);
    RAW(0x70, 9, 0, 0x96, 0x12c, 0x100, 0x10, 1, 0x20, S(0x1c), 9, 0, 0, 0, 0);
    xl(0x0057a400);
    RAW(0xa8, 5, 0, 0x96, 0x122, 0, 0, G(0x0057a404), 0, 0, sty(S_COL_4FC, S_COL_40C), 0, 0, 0, 0);
    xl(0x0057a558);
    IC(0xe0, 2, 0, 0x28, 0xf5, 0, 0, G(0x0057a55c), 0, F_MLC_Answer, 0, 0, 0, 0, 0);
    RAW(0x118, 1, 1, 0, 0, 0, 0, 0, 0, S(0x18), 0, 0, 0, 0, 0);
    item_end(fr.t(0x150));
    add_items(self, fr.t(0));
    tcall<void>(F_LC_Added, self);
}
PORT_FN(0x0048eb90, "ModemLineControl::Added", ModemLineControl_Added_n, fp_items0)

// Connect, in the controls' group; then LineControl's
static void __fastcall DirectLineControl_Added_n(LineControl* self, Edx) {
    Fr<0xe0> fr;
    once_xl(0x0057a46c, 1, 0x0057a4d0, 0x004f9000, 0x0048f050);
    RAW(0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x18), 0, 0, 0, 0, 0);
    xl(0x0057a4d0);
    RAW(0x38, 2, 0, 0x28, 0x11d, 0, 0, G(0x0057a4d4), 0, F_DLC_Connect, 0, 0, 0, 0, 0);
    RAW(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0x18), 0, 0, 0, 0, 0);
    item_end(fr.t(0xa8));
    add_items(self, fr.t(0));
    tcall<void>(F_LC_Added, self);
}
PORT_FN(0x0048eed0, "DirectLineControl::Added", DirectLineControl_Added_n, fp_items0)

// LineControl's Draw: nothing
static void __fastcall LineControl_Draw_n(LineControl*, Edx, gxCanvas*) {}
static void fp_LineControl_Draw(Footprint&, LineControl*, Edx, gxCanvas*) {}
PORT_FN(0x0048fa60, "LineControl::Draw", LineControl_Draw_n, fp_LineControl_Draw)

// the deleting destructors (the Modem's saves its number)
static void* __fastcall LineControl_vdd_n(LineControl* self, Edx, uint32_t flags) {
    tcall<void>(F_LineControl_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048fa70, "LineControl::vector deleting destructor", LineControl_vdd_n, fp_frees)

static void* __fastcall ModemLineControl_sdd_n(LineControl* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_ModemLineControl;
    ccall<void>(F_OptionsSetS, sec_multi(), (const char*)0x004f90c8, (const char*)&self->phone[0]);
    UI_GP(void, S_MLC_GLOBAL) = 0;
    tcall<void>(F_LineControl_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048fa90, "ModemLineControl::scalar deleting destructor", ModemLineControl_sdd_n, fp_frees)

static void* __fastcall DirectLineControl_vdd_n(LineControl* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_DirectLineControl;
    UI_GP(void, S_DLC_GLOBAL) = 0;
    tcall<void>(F_LineControl_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048fae0, "DirectLineControl::vector deleting destructor", DirectLineControl_vdd_n, fp_frees)

// =====================================================================================================================
// MenuMultiChooseTransport
// =====================================================================================================================
static MultiGenesisInfo* __fastcall MultiGenesisInfo_ctor_n(MultiGenesisInfo* self, Edx) {
    volatile uint32_t* d = (volatile uint32_t*)self;
    for (int i = 0; i < 0x1b; i++) d[i] = 0;
    return self;
}
static void fp_MultiGenesisInfo_ctor(Footprint& f, MultiGenesisInfo* self, Edx) { f.add(self, sizeof(MultiGenesisInfo), "this"); }
PORT_FN(0x0048f9f0, "MultiGenesisInfo::MultiGenesisInfo", MultiGenesisInfo_ctor_n, fp_MultiGenesisInfo_ctor)

// Down / Up (0x128 / 0x126): Lan -> Direct -> Modem -> Lan, and back
static uint8_t __cdecl next_tab_cb_n(int32_t) {
    const uint32_t lan = G(S_GRP_LAN);
    const uint32_t direct = G(S_GRP_DIRECT);
    if (G(S_TAB) == lan) { G(S_TAB) = direct; return 0; }
    if (G(S_TAB) == direct) { G(S_TAB) = G(S_GRP_MODEM); return 0; }
    G(S_TAB) = G(S_GRP_LAN);
    return 0;
}
static void fp_tab_cb(Footprint& f, int32_t) { UI_FP(f, S_TAB, 4, "mmulti.obj the tab chosen"); }
PORT_FN(0x0048f910, "next_tab_cb", next_tab_cb_n, fp_tab_cb)

static uint8_t __cdecl prev_tab_cb_n(int32_t) {
    const uint32_t lan = G(S_GRP_LAN);
    const uint32_t modem = G(S_GRP_MODEM);
    if (G(S_TAB) == lan) { G(S_TAB) = modem; return 0; }
    if (G(S_TAB) == modem) { G(S_TAB) = G(S_GRP_DIRECT); return 0; }
    G(S_TAB) = G(S_GRP_LAN);
    return 0;
}
PORT_FN(0x0048f950, "prev_tab_cb", prev_tab_cb_n, fp_tab_cb)

// The Multiplayer screen. Its frame, as the original's (fr.b + k = [esp + k] after the `sub esp, 0x86c`):
//   0 the UIDialog; 0x14 the NetBrowser; 0x94 the items (17, to 0x44c); 0x44c "Multi:Version 37" (0x40);
//   0x48c the ModemLineControl; 0x67c the DirectLineControl
// The choice: Ok (0) on the LAN tab joins the game chosen (its service copied, a fresh socket of the protocol); on a line
// tab, a socket on the device the call made (the server as the line check found); Create (1) a new game on a fresh socket.
// 0 if the line's socket couldn't be made (or the dialog ended so: -1), else 1.
static uint8_t __cdecl MenuMultiChooseTransport_n(MultiGenesisInfo* info) {
    Fr<0x86c> fr;
    NetBrowser* nb = (NetBrowser*)fr.t(0x14);
    LineControl* modem = (LineControl*)fr.t(0x48c);
    LineControl* direct = (LineControl*)fr.t(0x67c);
    char* const version = (char*)fr.t(0x44c);
    const char* sec = sec_multi();
    G(S_TAB) = 1;
    ccall<void>(F_OptionsGetI, sec, (const char*)0x004f9014, (void*)(uintptr_t)S_TAB);
    tcall<void*>(F_LineControl_ctor, modem, (uint32_t)1);
    UI_GP(void, S_MLC_GLOBAL) = modem;
    modem->vtbl = (const void*)(uintptr_t)VT_ModemLineControl;
    ccall<void>(F_OptionsGetS, sec_multi(), (const char*)0x004f90c8, (char*)&modem->phone[0], (int32_t)0x20);
    tcall<void*>(F_LineControl_ctor, direct, (uint32_t)2);
    direct->vtbl = (const void*)(uintptr_t)VT_DirectLineControl;
    UI_GP(void, S_DLC_GLOBAL) = direct;
    tcall<void*>(F_NetBrowser_ctor, nb);
    once_xl(0x0057a5c4, 1, 0x0057a4a0, 0x004f9020, 0x0048f9e0);
    once_xl(0x0057a5c4, 2, 0x0057a428, 0x004f9034, 0x0048f9d0);
    once_xl(0x0057a5c4, 4, 0x0057a2d8, 0x004f9044, 0x0048f9c0);
    once_xl(0x0057a5c4, 8, 0x0057a350, 0x004f9050, 0x0048f9b0);
    once_xl(0x0057a5c4, 0x10, 0x0057a4b0, 0x004f9060, 0x0048f9a0);
    once_xl(0x0057a5c4, 0x20, 0x0057a2e8, 0x004f906c, 0x0048f990);
    xl(0x0057a428);
    {
        // FIX: "<Multi:Version> 37" went into 0x40 bytes unbounded, before the line controls in the frame: a translation
        // over 60 characters keeps the text's first 63 (fx_len above)
        const char* s = UI_GP(const char, 0x0057a42c);
        if (VP_FIX && fx_len(s) + 1 + fx_dec((int32_t)multi_version()) > 0x3f) {
            char t[0x400], c1[0x100];
            UI_sprintf(t, (const char*)0x004f9074, fx_cut(s, c1, 0xff), multi_version());
            ui_copy_bounded(version, t, 0x40);
        } else {
            UI_sprintf(version, (const char*)0x004f9074, s, multi_version());
        }
    }
    IC(0x94, 4, 0x1e, 0x126, 0, 0, 0, 0, 0, F_prev_tab_cb, 0, 0, 0, 0, 0);
    IC(0xcc, 4, 0x1f, 0x128, 0, 0, 0, 0, 0, F_next_tab_cb, 0, 0, 0, 0, 0);
    IC(0x104, 1, 0, 0, 0, 0, 0, 0, 0, S_GRP_LAN, 0, 0, 0, 0, 0);
    RAW(0x13c, 0x17, 0, 0xf0, 0x96, 0x100, 0xc8, S_EMPTY, 0, U(nb), 0, 0, 0, 0, 0);
    IC(0x174, 1, 1, 0, 0, 0, 0, 0, 0, S_GRP_LAN, 0, 0, 0, 0, 0);
    IC(0x1ac, 1, 0, 0, 0, 0, 0, 0, 0, S_GRP_DIRECT, 0, 0, 0, 0, 0);
    RAW(0x1e4, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(direct), 0, 0, 0, 0, 0);
    IC(0x21c, 1, 1, 0, 0, 0, 0, 0, 0, S_GRP_DIRECT, 0, 0, 0, 0, 0);
    IC(0x254, 1, 0, 0, 0, 0, 0, 0, 0, S_GRP_MODEM, 0, 0, 0, 0, 0);
    RAW(0x28c, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(modem), 0, 0, 0, 0, 0);
    IC(0x2c4, 1, 1, 0, 0, 0, 0, 0, 0, S_GRP_MODEM, 0, 0, 0, 0, 0);
    xl(0x0057a2d8);
    RAW(0x2fc, 0xc, 0, 0x21, 0x58, (uint32_t)-1, 0, G(0x0057a2dc), S_GRP_LAN, S_TAB, 3, 0, 0, 0, 0);
    xl(0x0057a350);
    IC(0x334, 0xc, 0, 0xad, 0x58, (uint32_t)-1, 0, G(0x0057a354), S_GRP_DIRECT, S_TAB, 3, 0, 0, 0, 0);
    xl(0x0057a4b0);
    RAW(0x36c, 0xc, 0, 0x139, 0x58, (uint32_t)-1, 0, G(0x0057a4b4), S_GRP_MODEM, S_TAB, 3, 0, 0, 0, 0);
    xl(0x0057a2e8);
    IC(0x3a4, 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, G(0x0057a2ec), 0, 0, 6, 0, 0, 0, 0);
    IC(0x3dc, 5, 0, 0xf0, 0x162, 0, 0, U(version), 0, 0, sty(S_COL_4FC, S_COL_56C), 0, 0, 0, 0);
    item_end(fr.t(0x414));
    xl(0x0057a4a0);
    dialog(fr.t(0), G(0x0057a4a4), 0x004f907c, (uint32_t)-2, U(fr.t(0x94)), 0);
    int32_t r = ccall<int32_t>(F_UIDoDialog, fr.t(0), UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), (int32_t)-999, (int32_t)-999, (int32_t)1);
    if (r == 0) {
        if (G(S_TAB) != G(S_GRP_LAN)) {
            LineControl* line = direct;
            if (G(S_TAB) != G(S_GRP_DIRECT)) line = modem;
            void* dev = tcall<void*>(F_LC_GetDevice, line);
            void* s = ccall<void*>(F_SocketCreateLine, dev);
            info->sock = s;
            if (s) {
                info->named = 1;
                info->is_server = line->is_server;
                crt_strcpy((char*)&info->service[0], (const char*)0x004f9088);
            } else {
                UI_LogReport((const char*)0x004f9094);
                r = -1;
            }
        } else if (VP_FIX && nb->mgr[nb->proto] && !fx_sel_listed(nb->mgr[nb->proto], nb->sel)) {
            r = -1;                     // FIX: (connect) no game listed is chosen: as if the screen were left (Back)
        } else {
            const uint8_t* rs = tcall<const uint8_t*>(F_SessionMgr_GetServiceTable, nb->mgr[nb->proto]);
            crt_copy(&info->service[0], rs + (uint32_t)nb->sel * 0x54u, 0x54);
            tcall<void>(F_DestroySockets, nb);
            info->sock = ccall<void*>(F_CreateSocket, nb->proto);
            info->is_server = 0;
            info->named = 0;
            crt_strcpy(&info->password[0], &nb->password[0]);
        }
    } else if (r == 1) {
        tcall<void>(F_DestroySockets, nb);
        void* s = ccall<void*>(F_CreateSocket, nb->proto);
        info->is_server = 1;
        info->named = 1;
        info->sock = s;
        crt_strcpy((char*)&info->service[0], &nb->game_name[0]);
        crt_strcpy(&info->password[0], &nb->password[0]);
    }
    ccall<void>(F_OptionsSetI, sec_multi(), (const char*)0x004f90bc, (int32_t)G(S_TAB));
    tcall<void>(F_NetBrowser_dtor, nb);
    UI_GP(void, S_DLC_GLOBAL) = 0;
    direct->vtbl = (const void*)(uintptr_t)VT_DirectLineControl;
    tcall<void>(F_LineControl_dtor, direct);
    modem->vtbl = (const void*)(uintptr_t)VT_ModemLineControl;
    ccall<void>(F_OptionsSetS, sec_multi(), (const char*)0x004f90c8, (const char*)&modem->phone[0]);
    UI_GP(void, S_MLC_GLOBAL) = 0;
    tcall<void>(F_LineControl_dtor, modem);
    return r != -1 ? 1 : 0;
}
static void fp_MenuMultiChooseTransport(Footprint& f, MultiGenesisInfo*) {
    f.replay_only = "the Multiplayer screen: runs a dialog (modal), makes sockets and line devices, reads and writes the options";
}
PORT_FN(0x0048f060, "MenuMultiChooseTransport", MenuMultiChooseTransport_n, fp_MenuMultiChooseTransport)

#undef RAW
#undef IC
#undef S
#undef G
}  // namespace menu_multi
}  // namespace
