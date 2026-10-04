// menu_sched.cpp -- multiplayer stage N3 (group B): msched.obj, the lobby, rewritten faithfully (library `menu`).
//
//   MenuMultiScheduler (the session, the race server for the host, the race client made from the Multiplayer screen's
//   MultiGenesisInfo) and MultiDo (the lobby's dialog, built on its own stack: the chat, the track, the race info, the car,
//   the master, a LiveMultiKiller); ChatControl (the user list, whisper, the chat line; Speak / whisper send it in 47-byte
//   pieces, break_chatline) with its ChatWindow (the scrollback); MultiMaster (the client's state machine: Update runs a
//   table of state functions, sf_* when a state is entered and i_* each frame; Approve, Race, Back, Exit; the host's
//   proposal, sent 750 ms after the last change; the Approve button's glint), MultiCarChooser (Prev / Next / Setup, and
//   options_cb: Options -> Hacks -> Vehicle list, the only way to change cars, also in multiplayer), MultiTrackChooser,
//   MultiRaceCfg (the host's settings: realism, damage, time, weather, race type, AI cars), MultiRaceInfo (the proposal as
//   text), NumberString, UI_SLIDER, PostRace::Tick, the deleting destructors and the static callbacks.
//
// Written from the v1.0 disassembly, as hook/menu_options.cpp: every call in the original's order by its v1.0 address (the
// toolkit's, the multi library's -- now rewritten, but called by address exactly as the original does --, group C's
// CarViewer3D, this file's own), virtual calls through the vtable. Item lists are built in a frame laid out as the
// original's (the same offsets from its top), UIDialogItem's constructor called where the original calls it and every
// dword stored where it stores an item inline; the translated texts read after their Xlator is refreshed, in the
// original's order. MultiDo keeps its whole frame so: the five controls and the LiveMultiKiller sit where the original's
// do relative to each other and to the item list and the UIDialog.
//
// N0's patched call sites (hook/net_wsock.cpp): LiveMultiInfo::Grab at 0x48853b, 0x488ab1, 0x488b2c, 0x489959, 0x4899a0,
// 0x4899c9, 0x489a19, 0x48a9a9, 0x48a9d1, 0x48aa09, 0x48aa31, 0x48bf98, 0x48c62b and LiveMultiInfo::Release at 0x48855c,
// 0x488abe, 0x489966, 0x4899d6, 0x48a9b6, 0x48aa16, 0x48bf9f: the rewrites make the same calls through net_Grab /
// net_Release (hook/net_wsock.h), each marked with its site. (msched.obj has no PTimeNow / Random site: its PTimeNow
// calls are direct.)
//
// Footprints (main thread). replay_only, each with its reason: everything that talks to the race client / server / session
// (the multi library sends, ticks the session, reads the network's state), runs a dialog (MultiDo, the yes / no and lost
// connection boxes, MenuDoOptions, MenuEditCar), builds widgets (the Addeds), adds or removes notifications, reads or
// writes the options, loads or forgets stamps or cars (gxGetStamp, CarViewer3D), reads the clock (PTimeNow), frees, or
// constructs a function-local Xlator the first time (atexit). The rest list what they write: the object, the running
// window's groups (UIShowGroup / UIHideGroup), the canvas a draw is given, the item UI_SLIDER fills, the proposal a
// fillout fills; NumberString's Xlators once constructed; the stubs (a bare `ret`) are pure. Only ChatControl::Callback
// (0x488060) branches within its first 5 bytes: new or original, never shadowed.
//
// Fixes (// FIX:, docs/FIXES.md "Multiplayer menus"; VP_FIX, off in the faithful harness build): the track chooser holds
// every track (its 16-name table kept for the first 16, the rest read from the track table) and checks the host's track
// against this machine's; the lobby's state table checked (a state past car get ends the lobby as car get does); every
// text held to its buffer (the chat's scrollback line, the race settings' and the race info's texts, the ghost types'
// string, the lost-connection box, "<car>.car", the track's name, the game's name); NumberString outside 0..14 gives "";
// the show / hide table, no tracks, a text height of -3. See each `// FIX:`.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "net_wsock.h"
#include "menu_sched.h"

namespace {
namespace menu_sched {
using namespace uit;
using namespace msc;

#define D(x) ((double)(x))

// a frame laid out as the original's: at(n) is the original's [entry esp - n]
template <uint32_t N> struct Frame {
    alignas(4) uint8_t b[N];
    __forceinline void* at(uint32_t n) { return b + N - n; }
    __forceinline volatile uint32_t& d(uint32_t n) { return *(volatile uint32_t*)(b + N - n); }
    __forceinline volatile uint8_t& c(uint32_t n) { return *(volatile uint8_t*)(b + N - n); }
};
#define G(a) UI_GU32(a)
#define S(o) ((uint32_t)(uintptr_t)self + (uint32_t)(o))
static __forceinline uint32_t U(const volatile void* p) { return (uint32_t)(uintptr_t)p; }

// an Xlator refreshed (the cookie compared, Xlator::xlate called when stale), no value read
static __forceinline void xl(uint32_t x) {
    if (UI_GU32(x + 8) != UI_GU32(S_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (void*)(uintptr_t)x);
}
// a function-local static Xlator's guard: `test [guard], bit; jne; or ...; Xlator::Xlator; atexit(helper)`
static __forceinline void once_xl(uint32_t guard, uint8_t bit, uint32_t x, uint32_t key, uint32_t helper) {
    const uint8_t g = UI_G8(guard);
    if (g & bit) return;
    UI_G8(guard) = (uint8_t)(g | bit);
    tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)x, (const char*)(uintptr_t)key);
    ccall<int>(F_atexit, helper);
}
// the menus' colour-pair test of one colour against itself: `mov ecx, eax; sub ecx, eax; cmp ecx, 1; sbb; add 0xc` (0xb)
static __forceinline uint32_t sty1(uint32_t a) { const uint32_t v = UI_GU32(a); return v - v == 0 ? 0xbu : 0xcu; }
// UIDialogItem::UIDialogItem (replay.obj), by address: all 14 dwords as the original pushes them
static __forceinline void item_ctor(void* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    uint32_t text, uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi,
                                    uint32_t sel, uint32_t s34) {
    tcall<void*>(F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// an item the original stores inline: all 14 dwords
static __forceinline void raw(void* p, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t text,
                              uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi, uint32_t sel, uint32_t s34) {
    volatile uint32_t* d = (volatile uint32_t*)p;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = lo; d[11] = hi; d[12] = sel; d[13] = s34;
}
// the end of a list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void item_end(void* it) { crt_copy(it, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline void add_items(void* self, void* items) { tcall<void>(F_UICC_AddItems, self, items); }
#define IC(n, ...) item_ctor(fr.at(n), __VA_ARGS__)
#define RAW(n, ...) raw(fr.at(n), __VA_ARGS__)
// a group group shown or hidden through the table at 0x4f83d0 (UIHideGroup, UIShowGroup), read at the call
// FIX: an index other than 0 or 1 (only set_arrows / set_garage take one from their caller, a byte: the lobby passes 0 or
// 1) called what follows the two-entry table; any other is taken as 1 (show), as a flag would be
static __forceinline void showhide(uint32_t i, uint32_t grp) {
    if (VP_FIX && i > 1u) i = 1u;
    ((void(__cdecl*)(uint32_t))(uintptr_t)UI_GU32(S_SHOWHIDE + 4u * i))(grp);
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
// a string copied into a field of `size` bytes: held to it (the fix), or unbounded (the original)
static __forceinline void fx_copy(char* dst, const char* src, uint32_t size) {
    if (VP_FIX) ui_copy_bounded(dst, src, size);
    else crt_strcpy(dst, src);
}
// "%s: %s" (fmt) into a field of `size` bytes: as the original formats it, or (the fix) held to the field
static void fx_pair(char* field, uint32_t size, uint32_t fmt, const char* a, const char* b) {
    if (VP_FIX && fx_len(a) + 2 + fx_len(b) > size - 1) {
        char t[0x400], c1[0x100], c2[0x100];
        UI_sprintf(t, (const char*)(uintptr_t)fmt, fx_cut(a, c1, 0xff), fx_cut(b, c2, 0xff));
        ui_copy_bounded(field, t, size);
    } else {
        UI_sprintf(field, (const char*)(uintptr_t)fmt, a, b);
    }
}
static __forceinline void hide(uint32_t grp) { ccall<void>(F_UIHideGroup, grp); }
static __forceinline void show(uint32_t grp) { ccall<void>(F_UIShowGroup, grp); }
static __forceinline const char* sec(uint32_t s) { return UI_GP(const char, s); }
static __forceinline void opt_get_i(const char* s, uint32_t key, volatile void* p) { ccall<void>(F_OptionsGetI, s, (const char*)(uintptr_t)key, (void*)p); }
static __forceinline void opt_get_b(const char* s, uint32_t key, volatile void* p) { ccall<void>(F_OptionsGetB, s, (const char*)(uintptr_t)key, (void*)p); }
static __forceinline void opt_set_i(const char* s, uint32_t key, int32_t v) { ccall<void>(F_OptionsSetI, s, (const char*)(uintptr_t)key, v); }
// a byte argument: the original pushes the whole register (`mov al, [x]; push eax`); the callee reads only the byte
static __forceinline void opt_set_b(const char* s, uint32_t key, uint8_t v) { ccall<void>(F_OptionsSetB, s, (const char*)(uintptr_t)key, (uint32_t)v); }

// the singletons
static __forceinline MultiMaster* g_master() { return UI_GP(MultiMaster, S_MASTER_G); }
static __forceinline MultiCarChooser* g_carch() { return UI_GP(MultiCarChooser, S_CARCH_G); }
static __forceinline MultiTrackChooser* g_track() { return UI_GP(MultiTrackChooser, S_TRACKCH_G); }
static __forceinline MultiRaceCfg* g_cfg() { return UI_GP(MultiRaceCfg, S_RACECFG_G); }
static __forceinline MultiRaceInfo* g_info() { return UI_GP(MultiRaceInfo, S_RACEINFO_G); }
static __forceinline int32_t client_state(const void* c) { return *(const volatile int32_t*)((const uint8_t*)c + RC_STATE); }
static __forceinline void* lmi_ptr(const void* lmi, uint32_t off) { return *(void* const volatile*)((const uint8_t*)lmi + off); }

// ---- footprints ---------------------------------------------------------------------------------------------------------------
template <typename T> static void fp_client0(Footprint& f, T*, Edx) { f.replay_only = "talks to the race client / server (the multi library)"; }
template <typename T> static void fp_items0(Footprint& f, T*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates)"; }
template <typename T> static void fp_notes0(Footprint& f, T*, Edx) { f.replay_only = "adds or removes notifications (allocates or frees their copies)"; }
template <typename T> static void fp_frees(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object"; }
template <typename T> static void fp_pure_draw(Footprint& f, T*, Edx, gxCanvas*) { f.pure = true; }
static void fp_cb_client(Footprint& f, int32_t) { f.replay_only = "talks to the race client / server (the multi library)"; }
// the running window's groups shown or hidden (UIShowGroup / UIHideGroup: the window and its widgets' flags)
static void fp_groups(Footprint& f) { ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE)); }
// the current canvas made `c`, and what a draw clipped to it writes
static void fp_draw(Footprint& f, const void* c) {
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    UI_FP_CANVAS(f, c);
}

// =====================================================================================================================
// ChatControl, ChatWindow
// =====================================================================================================================
static ChatControl* __fastcall ChatControl_ctor_n(ChatControl* self, Edx, void* client) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    tcall<void*>(F_ChatWindow_ctor, &self->window, client);
    self->vtbl = (const void*)(uintptr_t)VT_ChatControl;
    UI_GP(ChatControl, S_CHAT_G) = self;
    self->client = client;
    self->dragging = 0;
    self->users = 0;
    self->sel_id = 0;
    self->sel = -1;
    self->stamp = ccall<void*>(F_gxGetStamp, (const char*)0x004f84c0);       // "multi_s.stp"
    *(volatile char*)self->text = 0;
    return self;
}
static void fp_ChatControl_ctor(Footprint& f, ChatControl*, Edx, void*) { f.replay_only = "loads a stamp (gxGetStamp)"; }
PORT_FN(0x00487d50, "ChatControl::ChatControl", ChatControl_ctor_n, fp_ChatControl_ctor)

static void __fastcall ChatControl_dtor_n(ChatControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_ChatControl;
    UI_GP(ChatControl, S_CHAT_G) = 0;
    void* st = self->stamp;
    if (st) ccall<void>(F_gxForgetStamp, st);
    tcall<void>(F_ChatWindow_dtor, &self->window);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_ChatControl_dtor(Footprint& f, ChatControl*, Edx) { f.replay_only = "forgets its stamp"; }
PORT_FN(0x00487dc0, "ChatControl::~ChatControl", ChatControl_dtor_n, fp_ChatControl_dtor)

// the user list: as many rows as fit, each the Nth user's name and, with the stamp, its status at the right edge
// FIX: a text height of -3 (a row 0 high) divided by 0; no rows are drawn
static void __fastcall ChatControl_Draw_n(ChatControl* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    const int32_t lh = ccall<int32_t>(F_gxTextHeight) + 3;
    int32_t y = self->y;
    const int32_t n = VP_FIX && lh == 0 ? 0 : self->h / lh;
    for (int32_t i = 0; i < n; i++) {
        const int32_t id = vcall<int32_t>(self->client, C_NTH_USER, i);
        if (id) {
            const char* name = vcall<const char*>(self->client, C_GET_USERNAME, id);
            ccall<void>(F_UIStyleDraw, (int32_t)0x12, self->x + 5, y, name, (uint32_t)0);
            if (self->stamp) {
                const int32_t st = vcall<int32_t>(self->client, C_GET_USER_STATUS, id);
                void* stamp = self->stamp;
                const int32_t x = self->w + self->x - 8;
                ccall<void>(F_gxDrawStamp, (const void*)stamp, x, y, st, (const void*)0);
            }
        }
        y += lh;
    }
}
static void fp_ChatControl_Draw(Footprint& f, ChatControl*, Edx, gxCanvas*) { f.replay_only = "asks the race client for its users (the multi library)"; }
PORT_FN(0x00487e00, "ChatControl::Draw", ChatControl_Draw_n, fp_ChatControl_Draw)

// the chat line said, a piece of up to 47 characters at a time (break_chatline), and cleared
static uint8_t __fastcall ChatControl_speak_n(ChatControl* self, Edx) {
    char* p = self->text;
    if (*(volatile char*)p) {
        do {
            char* next = ccall<char*>(F_break_chatline, p);
            vcall<void>(self->client, C_SPEAK, (const char*)p);
            p = next;
        } while (*(volatile char*)p);
        *(volatile char*)self->text = 0;
    }
    return 0;
}
PORT_FN(0x00487eb0, "ChatControl::speak", ChatControl_speak_n, fp_client0)

// a chat line cut for sending: the rest after the last space, tab or newline among its first 47 characters (that one made
// the end of the first piece), or its 47th character (the line not cut); a line of 47 or fewer is all one piece
static char* __cdecl break_chatline_n(char* s) {
    const int32_t n = (int32_t)crt_strlen(s);
    if (n <= 0x2f) return s + n;
    char* brk = 0;
    char* const end = s + 0x2f;
    for (char* p = s; p < end; p++) {
        const char ch = *(volatile char*)p;
        if (ch == ' ' || ch == 9 || ch == 10) brk = p;
    }
    if (brk) {
        *(volatile char*)brk = 0;
        return brk + 1;
    }
    return end;
}
static void fp_break_chatline(Footprint& f, char* s) {
    if (s) f.add(s, crt_strlen(s) + 1, "the chat line");
}
PORT_FN(0x00487ef0, "break_chatline", break_chatline_n, fp_break_chatline)

// the chat line whispered to the picked user, in pieces; cleared, and the pick forgotten
static uint8_t __fastcall ChatControl_whisper_n(ChatControl* self, Edx) {
    if (self->sel != -1) {
        const int32_t id = vcall<int32_t>(self->client, C_NTH_USER, self->sel);
        if (!id) {
            UI_LogReport((const char*)0x004f84cc, self->sel);                 // "Whisper(): Can't get selected user (%d)"
        } else if (*(volatile char*)self->text) {
            char* p = self->text;
            do {
                char* next = ccall<char*>(F_break_chatline, p);
                vcall<void>(self->client, C_WHISPER, (const char*)p, id);
                p = next;
            } while (*(volatile char*)p);
        }
    } else {
        UI_LogReport((const char*)0x004f84f4);                                  // "Whisper(): No selected element."
    }
    *(volatile char*)self->text = 0;
    self->sel = -1;
    return 0;
}
PORT_FN(0x00487f40, "ChatControl::whisper", ChatControl_whisper_n, fp_client0)

// dragging over the user list picks a row (clamped to the users), and remembers its user's id
// FIX: a text height of -3 (rows 0 high) divided by 0; the first row is picked
static void __fastcall ChatControl_MouseMove_n(ChatControl* self, Edx, int32_t, int32_t y) {
    if (!self->dragging) return;
    tcall<void>(F_UICC_Dirty, self);
    const int32_t lh = ccall<int32_t>(F_gxTextHeight) + 3;
    const int32_t max = vcall<int32_t>(self->client, C_COUNT_USERS) - 1;
    const int32_t q = VP_FIX && lh == 0 ? 0 : (y - self->y) / lh;
    if (q < 0) self->sel = 0;
    else self->sel = max < q ? max : q;
    self->sel_id = vcall<int32_t>(self->client, C_NTH_USER, self->sel);
}
static void fp_ChatControl_MouseMove(Footprint& f, ChatControl*, Edx, int32_t, int32_t) { f.replay_only = "asks the race client for its users (the multi library)"; }
PORT_FN(0x00487fc0, "ChatControl::MouseMove", ChatControl_MouseMove_n, fp_ChatControl_MouseMove)

static void __fastcall ChatControl_Create_n(ChatControl* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->text, (uint32_t)0x80);
    vcall<void>(self, 0x10, (int32_t)0, (const void*)self->text);                // Callback(0, text)
}
PORT_FN(0x00488020, "ChatControl::Create", ChatControl_Create_n, fp_notes0)

static void __fastcall ChatControl_Destroy_n(ChatControl* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)self->text);
}
PORT_FN(0x00488050, "ChatControl::Destroy", ChatControl_Destroy_n, fp_notes0)

// the Whisper button shown while there's a line and a user picked
static void __fastcall ChatControl_Callback_n(ChatControl* self, Edx, int32_t, const void*) {
    const uint32_t i = (*(volatile char*)self->text && self->sel != -1) ? 1u : 0u;
    showhide(i, self->grp_whisper);
}
static void fp_ChatControl_Callback(Footprint& f, ChatControl*, Edx, int32_t, const void*) { fp_groups(f); }
PORT_FN(0x00488060, "ChatControl::Callback", ChatControl_Callback_n, fp_ChatControl_Callback)

// a new user count redraws and finds the picked user again; a picked user is whispered to (the line, if any)
static void __fastcall ChatControl_Update_n(ChatControl* self, Edx) {
    const int32_t n = vcall<int32_t>(self->client, C_COUNT_USERS);
    if (self->users != n) {
        self->users = n;
        tcall<void>(F_UICC_Dirty, self);
        tcall<void>(F_ChatControl_keep_user_focused, self);
    }
    if (vcall<uint8_t>(self->client, C_ANY_USER_CHANGES)) tcall<void>(F_UICC_Dirty, self);
    if (self->sel != -1) tcall<uint8_t>(F_ChatControl_whisper, UI_GP(ChatControl, S_CHAT_G));
}
PORT_FN(0x00488090, "ChatControl::Update", ChatControl_Update_n, fp_client0)

// the picked user's row found again by its id (-1 if it's gone)
static void __fastcall ChatControl_keep_user_focused_n(ChatControl* self, Edx) {
    volatile int32_t it;
    int32_t i = 0;
    int32_t id = vcall<int32_t>(self->client, C_FIRST_USER, (int32_t*)&it);
    if (id) {
        while (self->sel_id != id) {
            i++;
            id = vcall<int32_t>(self->client, C_NEXT_USER, (int32_t*)&it);
            if (!id) break;
        }
        if (id) {
            self->sel = i;
            return;
        }
    }
    self->sel = -1;
}
PORT_FN(0x004880e0, "ChatControl::keep_user_focused", ChatControl_keep_user_focused_n, fp_client0)

// the scrollback (its ChatWindow), the chat line (an Input of 0x80), Enter says it (a hot key: Speak), the Whisper button's
// group
static void __fastcall ChatControl_Added_n(ChatControl* self, Edx) {
    Frame<0x150> fr;
    once_xl(S_CHAT_ONCE, 1, 0x0057a0c0, 0x004f8514, 0x00488350);              // "Multi:WhisperButton" (never read here)
    RAW(0x150, 0x17, 0, 0x19, 0x131, 0x1d5, 0x50, S_EMPTY_STR, 0, S(0xa8), 0, 0, 0, 0, 0);
    IC(0x118, 9, 0, 0x19, 0x185, 0x1d5, 0xa, 1, (uint32_t)-0x80, S(0x24), 9, 0, 0, 0, 0);
    RAW(0xe0, 4, 0, 0xd, 0, 0, 0, 0, 0, F_ChatControl_Speak, 0, 0, 0, 0, 0);
    RAW(0xa8, 1, 0, 0, 0, 0, 0, 0, 0, S(0xd0), 0, 0, 0, 0, 0);
    RAW(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0xd0), 0, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x150));
}
PORT_FN(0x00488130, "ChatControl::Added", ChatControl_Added_n, fp_items0)

static ChatWindow* __fastcall ChatWindow_ctor_n(ChatWindow* self, Edx, void* client) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->vtbl = (const void*)(uintptr_t)VT_ChatWindow;
    self->client = client;
    self->last = 0;
    return self;
}
static void fp_ChatWindow_ctor(Footprint& f, ChatWindow* self, Edx, void*) { f.add(self, sizeof(ChatWindow), "this"); }
PORT_FN(0x00488360, "ChatWindow::ChatWindow", ChatWindow_ctor_n, fp_ChatWindow_ctor)

static void __fastcall ChatWindow_dtor_n(ChatWindow* self, Edx) {
    self->client = 0;
    self->vtbl = (const void*)(uintptr_t)VT_ChatWindow;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_ChatWindow_dtor(Footprint& f, ChatWindow* self, Edx) { f.add(self, sizeof(ChatWindow), "this"); }
PORT_FN(0x00488380, "ChatWindow::~ChatWindow", ChatWindow_dtor_n, fp_ChatWindow_dtor)

static void __fastcall ChatWindow_Update_n(ChatWindow* self, Edx) {
    const void* p = vcall<const void*>(self->client, C_GET_CHAT_SCROLLBACK);
    if (self->last != p) tcall<void>(F_UICC_Dirty, self);
    self->last = p;
}
PORT_FN(0x004883a0, "ChatWindow::Update", ChatWindow_Update_n, fp_client0)

// the scrollback from the newest line up: "<name padded to the longest>: <text>" for a speaker's first line, " " for the
// lines that follow it (and 4 pixels more above a speaker's first), whispers in style 0x12's flag 2
// FIX: the line was sprintf'd unbounded into the frame (252 bytes from it to the frame's end -- the rewrite's buffer is that
// size): the name padding (MaxNameLength, the longest user's name) and a line of 0x32 fit, a padding (or a name, or a line
// that doesn't end in its field) past them ran over the return address. Such a line is made with the padding held to 256
// and the name and text cut to 255, and keeps its first 251 characters (fx_len above); every other is as before.
static void __fastcall ChatWindow_Draw_n(ChatWindow* self, Edx, gxCanvas* c) {
    char buf[0xfc];                                     // the original's frame from the line to its end: 252 bytes
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    const uint8_t* const first = vcall<const uint8_t*>(self->client, C_GET_CHAT_SCROLLBACK);
    if (!first) return;
    const int32_t maxlen = vcall<int32_t>(self->client, C_MAX_NAME_LENGTH);
    const int32_t lh = ccall<int32_t>(F_gxTextHeight) + 1;
    const int32_t ytop = self->y;
    const int32_t x = self->x + 3;
    int32_t y = self->h - lh + ytop - 1;
    const uint8_t* line = first;
    for (;;) {
        const uint8_t* prev = *(const uint8_t* const volatile*)(line + 0x44);
        uint8_t head;
        if (first == prev) head = 1;
        else head = *(const volatile int32_t*)line == *(const volatile int32_t*)prev ? 0 : 1;
        const char* name = head ? (const char*)(line + 0x36) : (const char*)0x004f8528;     // ""
        const int32_t ch = head ? ':' : ' ';
        const char* text = (const char*)(line + 4);
        if (VP_FIX) {
            const int64_t w = maxlen < 0 ? -(int64_t)maxlen : (int64_t)maxlen;
            const int64_t nl = (int64_t)fx_len(name);
            if ((w > nl ? w : nl) + 2 + (int64_t)fx_len(text) > 0xfb) {
                char t[0x400], c1[0x100], c2[0x100];
                const int32_t wc = maxlen < -0x100 ? -0x100 : maxlen > 0x100 ? 0x100 : maxlen;
                UI_sprintf(t, (const char*)0x004f852c, wc, fx_cut(name, c1, 0xff), ch, fx_cut(text, c2, 0xff));
                ui_copy_bounded(buf, t, sizeof buf);
            } else {
                UI_sprintf(buf, (const char*)0x004f852c, maxlen, name, ch, text);
            }
        } else {
            UI_sprintf(buf, (const char*)0x004f852c, maxlen, name, ch, text);   // "%*s%c %s"
        }
        const uint32_t flags = *(const volatile uint8_t*)(line + 0x43) ? 2u : 0u;
        ccall<void>(F_UIStyleDraw, (int32_t)0x12, x, y, (const char*)buf, flags);
        line = *(const uint8_t* const volatile*)(line + 0x44);
        if (head) y -= 4;
        y -= lh;
        if (first == line) return;
        if (!(y > ytop)) return;
    }
}
static void fp_ChatWindow_Draw(Footprint& f, ChatWindow*, Edx, gxCanvas*) { f.replay_only = "asks the race client for the scrollback (the multi library)"; }
PORT_FN(0x004883c0, "ChatWindow::Draw", ChatWindow_Draw_n, fp_ChatWindow_Draw)

// =====================================================================================================================
// MultiMaster
// =====================================================================================================================
// the host's race configuration (MemAlloc'd), the network's objects, the proposal's slaved-cars flag (under the lobby's
// lock: Grab, Release)
static MultiMaster* __fastcall MultiMaster_ctor_n(MultiMaster* self, Edx, void* lmi) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    if (lmi_ptr(lmi, LMI_SERVER)) {
        void* p = ccall<void*>(F_MemAlloc, (int32_t)0x8c);
        if (p) self->cfg = tcall<void*>(F_MultiRaceCfg_ctor, p);
        else self->cfg = 0;
    } else {
        self->cfg = 0;
    }
    self->vtbl = (const void*)(uintptr_t)VT_MultiMaster;
    self->sm = lmi_ptr(lmi, LMI_SM);
    self->client = lmi_ptr(lmi, LMI_CLIENT);
    self->server = lmi_ptr(lmi, LMI_SERVER);
    self->lmi = lmi;
    UI_GP(MultiMaster, S_MASTER_G) = self;
    self->done = 0;
    self->last_state = 0;
    for (int i = 0; i < 0xf; i++) ((volatile uint32_t*)self->proposal)[i] = 0;        // rep stosd
    net_Grab(lmi, 0);                                                            // site 0x48853b
    if (vcall<const uint8_t*>(self->client, C_GET_PROPOSAL)) {
        const uint8_t* p = vcall<const uint8_t*>(self->client, C_GET_PROPOSAL);
        *(volatile uint8_t*)&self->proposal[NP_IROC] = *(const volatile uint8_t*)(p + NP_IROC);
    }
    net_Release(lmi, 0);                                                         // site 0x48855c
    self->config_dirty = 1;
    self->config_time = 0;
    self->approve_time = 0;
    self->glint_frame = 0;
    self->approved = 0;
    return self;
}
static void fp_MultiMaster_ctor(Footprint& f, MultiMaster*, Edx, void*) { f.replay_only = "allocates the race configuration, locks the lobby (the multi library)"; }
PORT_FN(0x004884c0, "MultiMaster::MultiMaster", MultiMaster_ctor_n, fp_MultiMaster_ctor)

static void __fastcall MultiMaster_dtor_n(MultiMaster* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_MultiMaster;
    UI_GP(MultiMaster, S_MASTER_G) = 0;
    void* cfg = self->cfg;
    if (cfg) {
        vcall<void*>(cfg, 0, (uint32_t)1);                                       // the deleting destructor
        self->cfg = 0;
    }
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_MultiMaster_dtor(Footprint& f, MultiMaster*, Edx) { f.replay_only = "frees the race configuration"; }
PORT_FN(0x00488590, "MultiMaster::~MultiMaster", MultiMaster_dtor_n, fp_MultiMaster_dtor)

static void __fastcall MultiMaster_Create_n(MultiMaster* self, Edx) { tcall<void>(F_MultiMaster_hide_everything, self); }
static void fp_MultiMaster_Create(Footprint& f, MultiMaster*, Edx) { fp_groups(f); }
PORT_FN(0x004885c0, "MultiMaster::Create", MultiMaster_Create_n, fp_MultiMaster_Create)

// the yes / no and lost-connection boxes' idle function: the session, the client and the server ticked
static uint8_t __cdecl MultiMaster_Tick_n(int32_t*) {
    tcall<void>(F_MultiMaster_tick, g_master());
    return 0;
}
static void fp_MultiMaster_Tick(Footprint& f, int32_t*) { f.replay_only = "ticks the session, the race client and server (the multi library)"; }
PORT_FN(0x004885d0, "MultiMaster::Tick", MultiMaster_Tick_n, fp_MultiMaster_Tick)

static void __fastcall MultiMaster_tick_n(MultiMaster* self, Edx) {
    tcall<void>(F_SM_Tick, self->sm);
    vcall<void>(self->client, C_TICK);
    void* srv = self->server;
    if (srv) vcall<void>(srv, S_TICK);
}
PORT_FN(0x004885e0, "MultiMaster::tick", MultiMaster_tick_n, fp_client0)

// MultiMaster::Update's table, by the client's state: {entered, each frame, left} (the original builds it on its stack each
// call). FIX: a state past 13 (the race's: race waiting 0xe .. racing 0x15) or negative read past the table and called what
// it found there (state 14's "entered" is the return address). The lobby's dialog ends at 13 (car get), but the client can
// pass it within one tick: CommenceCarGet makes it 13 and, when it already has every car it's to fetch, 14 at once. A
// state past 13 now takes 13's functions (car get: the lobby ends, and MultiDo goes on to the race), a negative one 0's
// (invalid).
static const uint32_t k_state_fns[14 * 3] = {
    0, F_sf_hang, 0,                  // 0 invalid
    F_sf_noconn, 0, 0,                // 1
    F_sf_noconn, 0, 0,                // 2
    F_sf_noconn, 0, 0,                // 3
    F_sf_hang, 0, 0,                  // 4
    F_sf_preconn, 0, 0,               // 5 searching
    F_sf_preconn, 0, 0,               // 6 connecting
    F_sf_hang, 0, 0,                  // 7
    F_sf_preconn, 0, 0,               // 8 hello
    F_sf_running, F_i_running, 0,     // 9 running
    F_sf_proceeding, F_i_proceeding, 0,   // 10 proceeding
    F_sf_carchoice, F_i_carchoice, 0,     // 11 car choice
    F_sf_carwaiting, F_i_carwaiting, 0,   // 12 car waiting
    F_sf_carget, 0, 0,                // 13 car get
};
// each frame: the network ticked; the state's functions; the host's proposal sent 750 ms after its last change; the time
// approval became possible (for the glint)
static void __fastcall MultiMaster_Update_n(MultiMaster* self, Edx) {
    tcall<void>(F_MultiMaster_tick, self);
    const int32_t st = client_state(self->client);
    const int32_t old = self->last_state;
    // FIX: (above) the table's row for a state
    const int32_t rs = VP_FIX ? (st < 0 ? 0 : st > 13 ? 13 : st) : st;
    if (st != old) {
        const int32_t ro = VP_FIX ? (old < 0 ? 0 : old > 13 ? 13 : old) : old;
        const uint32_t ex = k_state_fns[ro * 3 + 2];
        if (ex) tcall<void>(ex, self);
        const uint32_t en = k_state_fns[rs * 3];
        if (en) tcall<void>(en, self);
    }
    {
        const uint32_t fr = k_state_fns[rs * 3 + 1];
        if (fr) tcall<void>(fr, self);
    }
    self->last_state = st;
    const int32_t t = self->config_time;
    if (t != 0 && (int32_t)((uint32_t)ccall<int32_t>(F_PTimeNow) - 0x2eeu) > t && vcall<int32_t>(self->client, C_MY_USER_ID)) {
        void* srv = self->server;
        const int32_t me = vcall<int32_t>(self->client, C_MY_USER_ID);
        vcall<void>(srv, S_PROPOSE, (const void*)self->proposal, me);
        self->config_time = 0;
    }
    bool possible;
    if (vcall<uint8_t>(self->client, C_I_AM_HOLDING_THINGS_UP)) possible = true;
    else if (self->config_time != 0 || self->approved != 0) possible = false;
    else {
        void* srv = self->server;
        possible = srv && vcall<uint8_t>(srv, S_ALL_APPROVED);
    }
    if (possible) {
        if (self->approve_time == 0) self->approve_time = ccall<int32_t>(F_PTimeNow);
    } else {
        self->approve_time = 0;
    }
    tcall<void>(F_MultiMaster_update_glint, self);
}
PORT_FN(0x00488610, "MultiMaster::Update", MultiMaster_Update_n, fp_client0)

// 5 s after approval became possible the Approve button glints: shown every other 2.4 s, its frame the time / 100 mod 8
static void __fastcall MultiMaster_update_glint_n(MultiMaster* self, Edx) {
    const int32_t now = ccall<int32_t>(F_PTimeNow);
    const int32_t t = self->approve_time;
    if (t != 0 && (int32_t)((uint32_t)now - (uint32_t)t) > 0x1388) {
        const uint32_t g = self->grp_glint;
        if ((now / 0x960) & 1) {
            show(g);
            self->glint_frame = (now / 0x64) % 8;
        } else {
            hide(g);
        }
        return;
    }
    hide(self->grp_glint);
}
static void fp_MultiMaster_update_glint(Footprint& f, MultiMaster*, Edx) { f.replay_only = "reads the clock (PTimeNow)"; }
PORT_FN(0x00488810, "MultiMaster::update_glint", MultiMaster_update_glint_n, fp_MultiMaster_update_glint)

// the host's car handed to the server when the cars are slaved
static void __fastcall MultiMaster_car_changed_n(MultiMaster* self, Edx) {
    MultiCarChooser* cc = g_carch();
    const int32_t idx = *(volatile int32_t*)(cc->viewer + CV_CAR);
    const char* name = tcall<const char*>(F_CarViewer3D_GetName, cc->viewer);
    void* srv = self->server;
    if (srv && *(volatile uint8_t*)&self->proposal[NP_IROC]) vcall<void>(srv, S_SET_CAR, idx, name);
}
PORT_FN(0x00488890, "MultiMaster::car_changed", MultiMaster_car_changed_n, fp_client0)

// the host changed the race: the proposal filled from the configuration and the track, shown, sent later (Update)
static void __fastcall MultiMaster_config_changed_n(MultiMaster* self, Edx) {
    self->config_dirty = 0;
    tcall<void>(F_MRC_fillout, g_cfg(), (void*)self->proposal);
    tcall<void>(F_MTC_fillout, g_track(), (void*)self->proposal);
    ccall<void>(F_MRI_UpdateProposal, (const void*)self->proposal);
    hide(self->grp_approve);
    self->config_time = ccall<int32_t>(F_PTimeNow);
}
static void fp_MultiMaster_config_changed(Footprint& f, MultiMaster*, Edx) { f.replay_only = "reads the clock (PTimeNow)"; }
PORT_FN(0x004888c0, "MultiMaster::config_changed", MultiMaster_config_changed_n, fp_MultiMaster_config_changed)

// Exit: the host is asked to be sure (a yes / no box, the network ticking under it); a client just leaves
static uint8_t __fastcall MultiMaster_exit_button_n(MultiMaster* self, Edx) {
    once_xl(S_EXIT_ONCE, 1, 0x00579fd8, 0x004f8538, 0x004889d0);              // "MultiServer:ExitDialogTitle"
    once_xl(S_EXIT_ONCE, 2, 0x0057a138, 0x004f8554, 0x004889c0);              // "MultiServer:ExitDialogAreYouSurePrompt"
    if (!self->server) return 1;
    xl(0x0057a138);
    xl(0x00579fd8);
    const uint32_t prompt = G(0x0057a13c);
    const uint32_t title = G(0x00579fdc);
    return ccall<uint8_t>(F_UIDoYesNoBox, (const char*)(uintptr_t)title, (const char*)(uintptr_t)prompt,
                          (uint32_t)F_MultiMaster_Tick);
}
static void fp_MultiMaster_exit_button(Footprint& f, MultiMaster*, Edx) { f.replay_only = "a yes / no box (modal)"; }
PORT_FN(0x00488910, "MultiMaster::exit_button", MultiMaster_exit_button_n, fp_MultiMaster_exit_button)

// Back: waiting for the others' cars, back to choosing; choosing a car, the approval withdrawn (the host's)
static uint8_t __fastcall MultiMaster_back_to_x_n(MultiMaster* self, Edx) {
    const int32_t st = client_state(self->client);
    if (st == 0xc) {
        hide(self->grp_track);
        vcall<void>(self->client, C_BACK_TO_CHOOSE_CAR);
        return 0;
    }
    if (st == 0xb) {
        void* srv = self->server;
        if (srv) vcall<void>(srv, S_UNAPPROVE);
        hide(self->grp_car);
        self->approved = 0;
    }
    return 0;
}
PORT_FN(0x004889e0, "MultiMaster::back_to_x", MultiMaster_back_to_x_n, fp_client0)

// Approve: the host approves its proposal; a client in the lobby proceeds to the cars
static uint8_t __fastcall MultiMaster_approve_n(MultiMaster* self, Edx) {
    void* srv = self->server;
    if (srv) {
        self->approved = 1;
        vcall<void>(srv, S_APPROVE);
        hide(self->grp_approve);
        return 0;
    }
    void* c = self->client;
    const int32_t st = client_state(c);
    if (st == 9) {
        vcall<void>(c, C_PROCEED_TO_CHOOSE_CAR);
        return 0;
    }
    UI_LogReport((const char*)0x004f857c, st);         // "MultiMaster::Approve() called bogusly: status == (%d)"
    return 0;
}
PORT_FN(0x00488a30, "MultiMaster::approve", MultiMaster_approve_n, fp_client0)

// Race: the car chosen (its .car kept loaded while the client takes it), the lobby's lock held for the race
// FIX: "<car name>.car" was built in 32 bytes: a car name over 27 characters (the car list holds up to 31) overran the
// frame. The name now has room (and is held to it): the car's resource set is loaded by its whole name, as for any other.
static uint8_t __fastcall MultiMaster_race_n(MultiMaster* self, Edx) {
    char buf[VP_FIX ? 0x110 : 0x20];
    const int32_t st = client_state(self->client);
    if (st != 0xb) {
        UI_LogReport((const char*)0x004f85bc, st);     // "MultiMaster::Race() called bogusly: status == (%d)"
        return 0;
    }
    if (!tcall<uint8_t>(F_LMI_AmOwner, g_master()->lmi)) net_Grab(g_master()->lmi, 0);   // site 0x488ab1
    net_Release(g_master()->lmi, 0);                                                      // site 0x488abe
    fx_copy(buf, tcall<const char*>(F_CarViewer3D_GetName, g_carch()->viewer), sizeof buf - 5);   // FIX: (above)
    {
        char* e = buf + crt_strlen(buf);
        *(volatile uint32_t*)e = *(const volatile uint32_t*)(uintptr_t)0x004f85b4;         // ".car"
        *(volatile char*)(e + 4) = *(const volatile char*)(uintptr_t)0x004f85b8;
    }
    ccall<void>(F_ResourceSetMustLoad, (const char*)buf);
    net_Grab(g_master()->lmi, 0);                                                         // site 0x488b2c
    void* c = self->client;
    uint8_t* viewer = g_carch()->viewer;
    const int32_t pj = tcall<int32_t>(F_CarViewer3D_GetPaintJob, viewer);
    const int32_t arg = -1 - pj;
    const char* name = tcall<const char*>(F_CarViewer3D_GetName, viewer);
    vcall<void>(c, C_PROCEED_TO_RACE, name, arg);
    ccall<void>(F_ResourceSetUnload, (const char*)buf);
    hide(self->grp_car);
    return 0;
}
PORT_FN(0x00488a80, "MultiMaster::race", MultiMaster_race_n, fp_client0)

// the buttons and their groups: Approve (twice: the lobby's and the proceeding state's), Cancel track, Cancel car, Race,
// the waiting message, the glint; the host's race configuration
static void __fastcall MultiMaster_Added_n(MultiMaster* self, Edx) {
    Frame<0x540> fr;
    once_xl(S_MASTER_ONCE, 1, 0x0057a208, 0x004f85f0, 0x00489250);            // "Multi:CloseEntryButton"
    once_xl(S_MASTER_ONCE, 2, 0x0057a270, 0x004f8608, 0x00489240);            // "Multi:CancelTrackButton"
    once_xl(S_MASTER_ONCE, 4, 0x00579f80, 0x004f8620, 0x00489230);            // "Multi:CancelCarButton"
    once_xl(S_MASTER_ONCE, 8, 0x0057a260, 0x004f8638, 0x00489220);            // "Multi:ApproveButton"
    once_xl(S_MASTER_ONCE, 0x10, 0x00579f70, 0x004f864c, 0x00489210);         // "Multi:RaceButton"
    once_xl(S_MASTER_ONCE, 0x20, 0x0057a218, 0x004f8660, 0x00489200);         // "Multi:WaitingForConnectionMessage"
    once_xl(S_MASTER_ONCE, 0x40, 0x0057a170, 0x004f8684, 0x004891f0);         // "Multi:SetCarButton" (never read here)
    IC(0x540, 1, 0, 0, 0, 0, 0, 0, 0, S(0x74), 0, 0, 0, 0, 0);
    xl(0x0057a208);
    IC(0x508, 2, 0, 0x1e5, 0x1a5, 0, 0, G(0x0057a20c), 0, F_MultiMaster_Approve, 2, 0, 0, 0, 0);
    IC(0x4d0, 1, 1, 0, 0, 0, 0, 0, 0, S(0x74), 0, 0, 0, 0, 0);
    IC(0x498, 1, 0, 0, 0, 0, 0, 0, 0, S(0x78), 0, 0, 0, 0, 0);
    xl(0x0057a270);
    IC(0x460, 2, 0, 0x154, 0x1a5, 0, 0, G(0x0057a274), 0, F_MultiMaster_BackToX, 2, 0, 0, 0, 0);
    IC(0x428, 1, 1, 0, 0, 0, 0, 0, 0, S(0x78), 0, 0, 0, 0, 0);
    IC(0x3f0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x7c), 0, 0, 0, 0, 0);
    xl(0x00579f80);
    RAW(0x3b8, 2, 0, 0x154, 0x1a5, 0, 0, G(0x00579f84), 0, F_MultiMaster_BackToX, 2, 0, 0, 0, 0);
    IC(0x380, 1, 1, 0, 0, 0, 0, 0, 0, S(0x7c), 0, 0, 0, 0, 0);
    IC(0x348, 1, 0, 0, 0, 0, 0, 0, 0, S(0x68), 0, 0, 0, 0, 0);
    xl(0x0057a260);
    IC(0x310, 2, 0, 0x1e5, 0x1a5, 0, 0, G(0x0057a264), 0, F_MultiMaster_Approve, 2, 0, 0, 0, 0);
    IC(0x2d8, 1, 1, 0, 0, 0, 0, 0, 0, S(0x68), 0, 0, 0, 0, 0);
    IC(0x2a0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x6c), 0, 0, 0, 0, 0);
    xl(0x00579f70);
    RAW(0x268, 2, 0, 0x1e5, 0x1a5, 0, 0, G(0x00579f74), 0, F_MultiMaster_Race, 2, 0, 0, 0, 0);
    IC(0x230, 1, 1, 0, 0, 0, 0, 0, 0, S(0x6c), 0, 0, 0, 0, 0);
    IC(0x1f8, 1, 0, 0, 0, 0, 0, 0, 0, S(0x70), 0, 0, 0, 0, 0);
    xl(0x0057a218);
    IC(0x1c0, 5, 0, 0x140, 0xf0, 0, 0, G(0x0057a21c), 0, 0, 0x17, 0, 0, 0, 0);
    IC(0x188, 1, 1, 0, 0, 0, 0, 0, 0, S(0x70), 0, 0, 0, 0, 0);
    IC(0x150, 1, 0, 0, 0, 0, 0, 0, 0, S(0x80), 0, 0, 0, 0, 0);
    IC(0x118, 0xe, 0, 0x1e5, 0x1a5, 0, 0, 0x004f8698, 0, S(0x84), 0, 0, 0, 0, 0);   // "glint.stp", its frame
    IC(0xe0, 1, 1, 0, 0, 0, 0, 0, 0, S(0x80), 0, 0, 0, 0, 0);
    item_end(fr.at(0xa8));
    add_items(self, fr.at(0x540));
    if (self->server) {
        IC(0x70, 0x17, 0, 0, 0, 0, 0, S_EMPTY_STR, 0, U(self->cfg), 0, 0, 0, 0, 0);
        item_end(fr.at(0x38));
        add_items(self, fr.at(0x70));
    }
}
PORT_FN(0x00488ba0, "MultiMaster::Added", MultiMaster_Added_n, fp_items0)

// the state functions -----------------------------------------------------------------------------------------------------------
static void __fastcall sf_hang_n(MultiMaster* self, Edx) {
    UI_LogPanic((const char*)0x004f86a4, client_state(self->client));      // "HANG State Function called (state == %d)"
}
static void fp_sf_hang(Footprint& f, MultiMaster*, Edx) { f.replay_only = "LogPanic"; }
PORT_FN(0x00489260, "MultiMaster::sf_hang", sf_hang_n, fp_sf_hang)

// the connection lost (or never made): a box with the client's status (or why it went down), then the lobby ends
// FIX: the box's text ("Multi:LostConnection"'s translation, a newline, the status or why it went down -- translations) was
// sprintf'd into 128 bytes unbounded; a longer one keeps its first 127 characters (fx_len above)
static void __fastcall sf_noconn_n(MultiMaster* self, Edx) {
    char buf[0x80];
    const int32_t st = client_state(self->client);
    const char* s = ccall<const char*>(F_GetClientStatusString, st);
    if (st == 3 || st == 1) {
        const char* d = ccall<const char*>(F_ClientDisconnectString,
                                           *(const volatile int32_t*)((const uint8_t*)self->client + RC_REASON));
        if (d) s = d;
    }
    once_xl(S_NOCONN_ONCE, 1, 0x0057a088, 0x004f86d0, 0x004893a0);            // "Multi:LostConnectionDialogTitle"
    once_xl(S_NOCONN_ONCE, 2, 0x0057a2c0, 0x004f86f0, 0x00489390);            // "Multi:LostConnection"
    xl(0x0057a2c0);
    {
        const char* lost = UI_GP(const char, 0x0057a2c4);
        if (VP_FIX && fx_len(lost) + 1 + fx_len(s) > 0x7f) {
            char t[0x400], c1[0x100], c2[0x100];
            UI_sprintf(t, (const char*)0x004f8708, fx_cut(lost, c1, 0xff), fx_cut(s, c2, 0xff));
            ui_copy_bounded(buf, t, sizeof buf);
        } else {
            UI_sprintf(buf, (const char*)0x004f8708, lost, s);                  // "%s\n%s"
        }
    }
    xl(0x0057a088);
    ccall<void>(F_UIDoDratBox, UI_GP(const char, 0x0057a08c), (const char*)buf, (uint32_t)F_MultiMaster_Tick);
    g_master()->done = 1;
}
static void fp_sf_noconn(Footprint& f, MultiMaster*, Edx) { f.replay_only = "a message box (modal)"; }
PORT_FN(0x00489280, "MultiMaster::sf_noconn", sf_noconn_n, fp_sf_noconn)

static void __fastcall sf_preconn_n(MultiMaster* self, Edx) { show(self->grp_preconn); }
static void fp_master_groups(Footprint& f, MultiMaster*, Edx) { fp_groups(f); }
PORT_FN(0x004893b0, "MultiMaster::sf_preconn", sf_preconn_n, fp_master_groups)

// in the lobby, each frame. The host: proceeds to the cars once its proposal is out, shows its settings. A client: the
// host's proposal taken (and shown) when it changes
static void __fastcall i_running_n(MultiMaster* self, Edx) {
    if (self->server) {
        self->approved = 0;
        if (vcall<const void*>(self->client, C_GET_PROPOSAL)) vcall<void>(self->client, C_PROCEED_TO_CHOOSE_CAR);
        vcall<int32_t>(self->client, C_COUNT_USERS);                         // (its result unused)
        hide(self->grp_running);
        show(g_cfg()->grp_all);
        show(g_track()->grp_arrows);
        return;
    }
    const uint8_t* p = vcall<const uint8_t*>(self->client, C_GET_PROPOSAL);
    if (p) {
        if (crt_memcmp_ne(p, self->proposal, 0x3c)) {
            crt_copy(self->proposal, p, 0x3c);
            tcall<void>(F_MTC_set_track_p, g_track(), (const void*)self->proposal);
            ccall<void>(F_MRI_UpdateProposal, (const void*)self->proposal);
        }
        show(g_info()->grp_all);
        show(self->grp_running);
    } else {
        hide(g_info()->grp_all);
        hide(self->grp_running);
    }
}
PORT_FN(0x004893c0, "MultiMaster::i_running", i_running_n, fp_client0)

static void __fastcall sf_running_n(MultiMaster* self, Edx) {
    if (self->server && self->config_dirty) tcall<void>(F_MultiMaster_config_changed, g_master());
    tcall<void>(F_MultiMaster_hide_everything, self);
}
static void fp_sf_running(Footprint& f, MultiMaster* self, Edx) {
    if (self && self->server && self->config_dirty) { f.replay_only = "reads the clock (PTimeNow)"; return; }
    fp_groups(f);
}
PORT_FN(0x004894b0, "MultiMaster::sf_running", sf_running_n, fp_sf_running)

// proceeding: the Approve button while the host's proposal is out, unapproved, and every client agrees
static void __fastcall i_proceeding_n(MultiMaster* self, Edx) {
    if (self->config_time == 0 && self->approved == 0) {
        void* srv = self->server;
        if (srv && vcall<uint8_t>(srv, S_ALL_APPROVED)) {
            show(self->grp_approve);
            return;
        }
    }
    hide(self->grp_approve);
}
PORT_FN(0x004894e0, "MultiMaster::i_proceeding", i_proceeding_n, fp_client0)

static void __fastcall sf_proceeding_n(MultiMaster* self, Edx) { hide(self->grp_running); }
PORT_FN(0x00489530, "MultiMaster::sf_proceeding", sf_proceeding_n, fp_master_groups)

// choosing a car: the host's settings hidden, the car chooser shown (its arrows and Setup unless the cars are slaved and
// this isn't the host), the race as agreed shown
static void __fastcall sf_carchoice_n(MultiMaster* self, Edx) {
    if (self->server) {
        hide(g_cfg()->grp_all);
        hide(g_track()->grp_arrows);
        show(self->grp_car);
    }
    show(g_carch()->grp_all);
    tcall<void>(F_MultiMaster_car_changed, g_master());
    tcall<void>(F_MCC_update_car, g_carch());
    show(self->grp_carchoice);
    const uint8_t iroc = *(volatile uint8_t*)&self->proposal[NP_IROC];
    const uint8_t arrows = (iroc && !self->server) ? 0 : 1;
    const uint8_t garage = (iroc && !self->server) ? 0 : 1;
    const uint8_t* ri = vcall<const uint8_t*>(self->client, C_GET_RACE_INFO);
    show(g_info()->grp_all);
    tcall<void>(F_MRI_update_race, g_info(), (const void*)ri, (uint32_t)*(volatile uint8_t*)&self->proposal[NP_IROC]);
    tcall<void>(F_MTC_set_track, g_track(), *(const volatile int32_t*)(ri + NRI_TRACK));
    tcall<void>(F_MTC_set_text_from_race, g_track(), (const void*)ri);
    tcall<void>(F_MCC_set_arrows, g_carch(), (uint32_t)arrows);
    tcall<void>(F_MCC_set_garage, g_carch(), (uint32_t)garage);
}
PORT_FN(0x00489540, "MultiMaster::sf_carchoice", sf_carchoice_n, fp_client0)

// with the cars slaved, the car the client's been given shown
static void __fastcall SlaveCar_n(MultiMaster* self, Edx) {
    if (!*(volatile uint8_t*)&self->proposal[NP_IROC]) return;
    if (vcall<int32_t>(self->client, C_GET_CAR) == *(volatile int32_t*)(g_carch()->viewer + CV_CAR)) return;
    if (vcall<int32_t>(self->client, C_GET_CAR) == -1) return;
    const int32_t car = vcall<int32_t>(self->client, C_GET_CAR);
    tcall<void>(F_CarViewer3D_SetCar, g_carch()->viewer, car);
    tcall<void>(F_MCC_update_car, g_carch());
}
PORT_FN(0x00489650, "MultiMaster::SlaveCar", SlaveCar_n, fp_client0)

static void __fastcall i_carchoice_n(MultiMaster* self, Edx) { tcall<void>(F_MultiMaster_SlaveCar, self); }
PORT_FN(0x004896a0, "MultiMaster::i_carchoice", i_carchoice_n, fp_client0)

static void __fastcall sf_carwaiting_n(MultiMaster* self, Edx) {
    hide(self->grp_carchoice);
    show(self->grp_track);
    tcall<void>(F_MCC_set_arrows, g_carch(), (uint32_t)0);
    tcall<void>(F_MCC_set_garage, g_carch(), (uint32_t)0);
}
static void fp_sf_carwaiting(Footprint& f, MultiMaster*, Edx) {
    if (MultiCarChooser* cc = g_carch()) f.add(cc, sizeof(MultiCarChooser), "MultiCarChooser::g (its arrows and Setup flags)");
    fp_groups(f);
}
PORT_FN(0x004896b0, "MultiMaster::sf_carwaiting", sf_carwaiting_n, fp_sf_carwaiting)

static void __fastcall i_carwaiting_n(MultiMaster* self, Edx) { tcall<void>(F_MultiMaster_SlaveCar, self); }
PORT_FN(0x004896f0, "MultiMaster::i_carwaiting", i_carwaiting_n, fp_client0)

static void __fastcall sf_carget_n(MultiMaster*, Edx) { g_master()->done = 1; }
static void fp_sf_carget(Footprint& f, MultiMaster*, Edx) {
    if (MultiMaster* m = g_master()) f.add(m, sizeof(MultiMaster), "MultiMaster::g");
}
PORT_FN(0x00489700, "MultiMaster::sf_carget", sf_carget_n, fp_sf_carget)

static void __fastcall hide_everything_n(MultiMaster* self, Edx) {
    hide(self->grp_glint);
    hide(self->grp_approve);
    hide(self->grp_preconn);
    hide(self->grp_running);
    hide(self->grp_carchoice);
    hide(self->grp_track);
    hide(self->grp_car);
    if (self->server) hide(g_cfg()->grp_all);
    hide(g_info()->grp_all);
    hide(g_carch()->grp_all);
}
PORT_FN(0x00489710, "MultiMaster::hide_everything", hide_everything_n, fp_master_groups)

static void __fastcall singleton_check_n(MultiMaster*, Edx) {}
static void fp_pure0(Footprint& f, MultiMaster*, Edx) { f.pure = true; }
PORT_FN(0x004897b0, "MultiMaster::singleton_check", singleton_check_n, fp_pure0)

// =====================================================================================================================
// MultiCarChooser
// =====================================================================================================================
static void __fastcall MCC_set_arrows_n(MultiCarChooser* self, Edx, uint8_t on) {
    showhide(on, self->grp_arrows);
    self->arrows = on;
    tcall<void>(F_MCC_update_text_loc, self);
}
static void fp_MCC_set(Footprint& f, MultiCarChooser* self, Edx, uint8_t) {
    f.add(self, sizeof(MultiCarChooser), "this");
    fp_groups(f);
}
PORT_FN(0x004897c0, "MultiCarChooser::set_arrows", MCC_set_arrows_n, fp_MCC_set)

static void __fastcall MCC_set_garage_n(MultiCarChooser* self, Edx, uint8_t on) {
    showhide(on, self->grp_garage);
    self->garage = on;
    tcall<void>(F_MCC_update_text_loc, self);
}
PORT_FN(0x004897f0, "MultiCarChooser::set_garage", MCC_set_garage_n, fp_MCC_set)

static void __fastcall MCC_update_text_loc_n(MultiCarChooser* self, Edx) {
    const uint32_t b = (self->arrows == 0 && self->garage == 0) ? 1u : 0u;
    showhide(b, self->grp_text_a);
    showhide(b < 1 ? 1u : 0u, self->grp_text_b);
}
static void fp_MCC_groups(Footprint& f, MultiCarChooser*, Edx) { fp_groups(f); }
PORT_FN(0x00489820, "MultiCarChooser::update_text_loc", MCC_update_text_loc_n, fp_MCC_groups)

// FIX: the friendly name (the car's .tab) was copied unbounded into 34 bytes, over the CarViewer3D that follows; it keeps 33
static void __fastcall MCC_update_car_n(MultiCarChooser* self, Edx) {
    fx_copy(self->name, tcall<const char*>(F_CarViewer3D_GetFriendlyName, self->viewer), sizeof self->name);
}
static void fp_MCC_update_car(Footprint& f, MultiCarChooser*, Edx) { f.replay_only = "asks the CarViewer3D (group C's: its car loaded)"; }
PORT_FN(0x00489870, "MultiCarChooser::update_car", MCC_update_car_n, fp_MCC_update_car)

// the car: the hack's (Options -> Hacks -> Vehicle list) or the viper
static MultiCarChooser* __fastcall MultiCarChooser_ctor_n(MultiCarChooser* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    int32_t car;
    if (ccall<uint8_t>(F_HackEnabled)) car = ccall<int32_t>(F_HackGetCarIndex);
    else car = ccall<int32_t>(F_GetCarFileNumber, (const char*)0x004f8710);   // "viper"
    tcall<void*>(F_CarViewer3D_ctor, self->viewer, car, (uint32_t)F_GetMaxCarFileNames, (uint32_t)F_GetCarFileName);
    self->vtbl = (const void*)(uintptr_t)VT_MultiCarChooser;
    UI_GP(MultiCarChooser, S_CARCH_G) = self;
    return self;
}
static void fp_MultiCarChooser_ctor(Footprint& f, MultiCarChooser*, Edx) { f.replay_only = "builds a CarViewer3D (loads the car)"; }
PORT_FN(0x004898b0, "MultiCarChooser::MultiCarChooser", MultiCarChooser_ctor_n, fp_MultiCarChooser_ctor)

static void __fastcall MultiCarChooser_dtor_n(MultiCarChooser* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_MultiCarChooser;
    UI_GP(MultiCarChooser, S_CARCH_G) = 0;
    tcall<void>(F_CarViewer3D_dtor, self->viewer);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_MultiCarChooser_dtor(Footprint& f, MultiCarChooser*, Edx) { f.replay_only = "destroys its CarViewer3D (frees the car)"; }
PORT_FN(0x00489900, "MultiCarChooser::~MultiCarChooser", MultiCarChooser_dtor_n, fp_MultiCarChooser_dtor)

static void __fastcall MultiCarChooser_Create_n(MultiCarChooser* self, Edx) { hide(self->grp_all); }
PORT_FN(0x00489930, "MultiCarChooser::Create", MultiCarChooser_Create_n, fp_MCC_groups)

// Options (the race info's button): MenuDoOptions with the lobby's lock let go; the car the Hacks tab chose shown
static uint8_t __cdecl options_cb_n(int32_t) {
    if (!tcall<uint8_t>(F_LMI_AmOwner, g_master()->lmi)) net_Grab(g_master()->lmi, 0);   // site 0x489959
    net_Release(g_master()->lmi, 0);                                                      // site 0x489966
    ccall<void>(F_MenuDoOptions);
    if (ccall<uint8_t>(F_HackEnabled)) {
        const int32_t idx = ccall<int32_t>(F_HackGetCarIndex);
        tcall<void>(F_CarViewer3D_SetCar, g_carch()->viewer, idx);
        tcall<void>(F_MCC_update_car, g_carch());
    }
    net_Grab(g_master()->lmi, 0);                                                         // site 0x4899a0
    return 0;
}
static void fp_options_cb(Footprint& f, int32_t) { f.replay_only = "runs the Options screen (modal), the lobby's lock"; }
PORT_FN(0x00489940, "options_cb", options_cb_n, fp_options_cb)

// Setup: the car's setup editor for the agreed track
static uint8_t __cdecl MCC_SetupCar_n(int32_t) {
    if (!tcall<uint8_t>(F_LMI_AmOwner, g_master()->lmi)) net_Grab(g_master()->lmi, 0);   // site 0x4899c9
    net_Release(g_master()->lmi, 0);                                                      // site 0x4899d6
    const uint8_t* ri = vcall<const uint8_t*>(g_master()->client, C_GET_RACE_INFO);
    const char* track = ccall<const char*>(F_GetTrackName, *(const volatile int32_t*)(ri + NRI_TRACK));
    const char* car = tcall<const char*>(F_CarViewer3D_GetName, g_carch()->viewer);
    ccall<uint8_t>(F_MenuEditCar, car, track, (uint32_t)0, (uint32_t)0);
    net_Grab(g_master()->lmi, 0);                                                         // site 0x489a19
    return 0;
}
static void fp_MCC_SetupCar(Footprint& f, int32_t) { f.replay_only = "runs the car setup editor (modal), the lobby's lock"; }
PORT_FN(0x004899b0, "MultiCarChooser::SetupCar", MCC_SetupCar_n, fp_MCC_SetupCar)

// the car's groups (the chooser, Prev / Next, Setup, the texts), the CarViewer3D, its name
static void __fastcall MultiCarChooser_Added_n(MultiCarChooser* self, Edx) {
    Frame<0x348> fr;
    once_xl(S_CARCH_ONCE, 1, 0x0057a2a0, 0x004f8718, 0x00489d50);             // "CarChooser:GarageButton"
    IC(0x348, 1, 0, 0, 0, 0, 0, 0, 0, S(0x154), 0, 0, 0, 0, 0);
    IC(0x310, 1, 0, 0, 0, 0, 0, 0, 0, S(0x15c), 0, 0, 0, 0, 0);
    IC(0x2d8, 1, 1, 0, 0, 0, 0, 0, 0, S(0x15c), 0, 0, 0, 0, 0);
    IC(0x2a0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x158), 0, 0, 0, 0, 0);
    IC(0x268, 1, 1, 0, 0, 0, 0, 0, 0, S(0x158), 0, 0, 0, 0, 0);
    IC(0x230, 0x17, 0, 0x1b2, 0x52, 0xb3, 0x91, S_EMPTY_STR, 0, S(0x3c), 0, 0, 0, 0, 0);
    IC(0x1f8, 1, 0, 0, 0, 0, 0, 0, 0, S(0x14c), 0, 0, 0, 0, 0);
    IC(0x1c0, 3, 0, 0x1b0, 0xee, 0, 0, 0x004f8730, 0, F_MCC_PrevCar, 0, 0, 0, 0, 0);     // "prev.stp"
    IC(0x188, 3, 0, 0x23a, 0xee, 0, 0, 0x004f873c, 0, F_MCC_NextCar, 0, 0, 0, 0, 0);     // "next.stp"
    IC(0x150, 1, 1, 0, 0, 0, 0, 0, 0, S(0x14c), 0, 0, 0, 0, 0);
    IC(0x118, 1, 0, 0, 0, 0, 0, 0, 0, S(0x150), 0, 0, 0, 0, 0);
    xl(0x0057a2a0);
    RAW(0xe0, 2, 0, 0x1dc, 0xea, 0, 0, G(0x0057a2a4), 0, F_MCC_SetupCar, 1, 0, 0, 0, 0);
    IC(0xa8, 1, 1, 0, 0, 0, 0, 0, 0, S(0x150), 0, 0, 0, 0, 0);
    IC(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0x154), 0, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x348));
}
PORT_FN(0x00489a30, "MultiCarChooser::Added", MultiCarChooser_Added_n, fp_items0)

// =====================================================================================================================
// MultiTrackChooser
// =====================================================================================================================
// the tracks' names (all but the last two rows), the MULTI "track" option, the frame's stamp
// FIX: the names were copied unbounded, 13 bytes each, into room for 16: a 17th track (an add-on track's row in
// tracks.tab) ran over the stamps, the host flag (set just before: a client could get the host's arrows) and the groups,
// then MultiDo's next control; a name over 12 characters ran into the next name. The table now keeps the first 16 tracks,
// each held to its 13 bytes, and every track past them -- or whose name is 12 characters or more -- is read from the track
// table itself where it's used (fx_track_name), so the chooser offers every installed track. Up to 16 tracks with names
// of up to 11 characters (the stock game's), every call is the original's.
static MultiTrackChooser* __fastcall MultiTrackChooser_ctor_n(MultiTrackChooser* self, Edx, uint8_t host) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_MultiTrackChooser;
    self->widget = 0;
    UI_GP(MultiTrackChooser, S_TRACKCH_G) = self;
    self->host = host;
    self->saved = 0;
    opt_get_i(sec(S_SEC_MULTI), 0x004f8748, &self->saved);                   // "track"
    self->track = self->saved;
    self->count = ccall<int32_t>(F_GetTrackCount) - 2;
    {
        const int32_t t = self->track, n = self->count;
        if (t >= n || t < 0) self->track = 0;
        if (n > 0) {
            int32_t i = 0;
            char* dst = self->names[0];
            do {
                const char* nm = ccall<const char*>(F_GetTrackName, i);
                i++;
                fx_copy(dst, nm, 0xd);                                // FIX: (above) each held to its 13 bytes
                dst += 0xd;
            } while (self->count > i && !(VP_FIX && i >= 16));        // FIX: (above) the first 16
        }
    }
    self->stamp = 0;
    self->frame = ccall<void*>(F_gxGetStamp, (const char*)0x004f8750);       // "carfrm.stp"
    for (int i = 0; i < 0x19; i++) ((volatile uint32_t*)self->text)[i] = 0;  // rep stosd: the texts
    return self;
}
static void fp_MultiTrackChooser_ctor(Footprint& f, MultiTrackChooser*, Edx, uint8_t) { f.replay_only = "reads the options, loads a stamp"; }
PORT_FN(0x00489d60, "MultiTrackChooser::MultiTrackChooser", MultiTrackChooser_ctor_n, fp_MultiTrackChooser_ctor)

// FIX helper (the constructor's table, set_track, MultiRaceCfg::Callback): track t's name for the chooser -- its copy in
// the table when that's whole (one of the first 16, under 12 characters: the original's read, no call), else the track
// table's own (GetTrackName) -- or 0 for a track this machine's table doesn't have (outside 0 .. count - 1: the host's
// track, from the network, past this machine's tracks)
static const char* fx_track_name(const MultiTrackChooser* self, int32_t t) {
    if (t < 0 || t >= self->count) return 0;
    if (t < 16) {
        const char* n = self->names[t];
        if (ui_strnlen(n, 12) < 12) return n;
    }
    return ccall<const char*>(F_GetTrackName, t);
}

static void __fastcall MultiTrackChooser_dtor_n(MultiTrackChooser* self, Edx) {
    const int32_t saved = self->saved;
    self->vtbl = (const void*)(uintptr_t)VT_MultiTrackChooser;
    opt_set_i(sec(S_SEC_MULTI), 0x004f875c, saved);                          // "track"
    void* st = self->stamp;
    if (st) ccall<void>(F_gxForgetStamp, st);
    void* fr = self->frame;
    if (fr) ccall<void>(F_gxForgetStamp, fr);
    UI_GP(MultiTrackChooser, S_TRACKCH_G) = 0;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_MultiTrackChooser_dtor(Footprint& f, MultiTrackChooser*, Edx) { f.replay_only = "writes the options, forgets its stamps"; }
PORT_FN(0x00489e60, "MultiTrackChooser::~MultiTrackChooser", MultiTrackChooser_dtor_n, fp_MultiTrackChooser_dtor)

static void __fastcall MTC_fillout_n(MultiTrackChooser* self, Edx, uint8_t* p) {
    const int32_t t = self->track;
    *(volatile int32_t*)(p + NP_TRACK) = t;
    tcall<void>(F_MTC_set_text_from_proposal, self, (const void*)p);
}
static void fp_MTC_fillout(Footprint& f, MultiTrackChooser*, Edx, uint8_t*) { f.replay_only = "the track's friendly name (the front end's table)"; }
PORT_FN(0x00489ec0, "MultiTrackChooser::fillout", MTC_fillout_n, fp_MTC_fillout)

static void __fastcall MultiTrackChooser_Create_n(MultiTrackChooser* self, Edx) { tcall<void>(F_MTC_set_track, self, self->track); }
static void fp_MTC_stamp0(Footprint& f, MultiTrackChooser*, Edx) { f.replay_only = "loads the track's stamp"; }
PORT_FN(0x00489ee0, "MultiTrackChooser::Create", MultiTrackChooser_Create_n, fp_MTC_stamp0)

// the track's group; the host's arrows (buttons and the cursor keys) in theirs
static void __fastcall MultiTrackChooser_Added_n(MultiTrackChooser* self, Edx) {
    Frame<0x230> fr;
    IC(0xa8, 1, 0, 0, 0, 0, 0, 0, 0, S(0x168), 0, 0, 0, 0, 0);
    IC(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0x168), 0, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0xa8));
    if (self->host == 0) return;
    IC(0x230, 1, 0, 0, 0, 0, 0, 0, 0, S(0x164), 0, 0, 0, 0, 0);
    IC(0x1f8, 3, 1, 0x17, 0xee, 0, 0, 0x004f8764, 0, F_MTC_Prev, 0, 0, 0, 0, 0);          // "prev.stp"
    IC(0x1c0, 4, 1, 0x125, 0, 0, 0, 0, 0, F_MTC_Prev, 0, 0, 0, 0, 0);                    // the left arrow key
    RAW(0x188, 3, 2, 0xa1, 0xee, 0, 0, 0x004f8770, 0, F_MTC_Next, 0, 0, 0, 0, 0);        // "next.stp"
    RAW(0x150, 4, 2, 0x127, 0, 0, 0, 0, 0, F_MTC_Next, 0, 0, 0, 0, 0);                   // the right arrow key
    RAW(0x118, 1, 1, 0, 0, 0, 0, 0, 0, S(0x164), 0, 0, 0, 0, 0);
    item_end(fr.at(0xe0));
    add_items(self, fr.at(0x230));
}
PORT_FN(0x00489ef0, "MultiTrackChooser::Added", MultiTrackChooser_Added_n, fp_items0)

// the track's picture, a bar, its name centred
static void __fastcall MultiTrackChooser_Draw_n(MultiTrackChooser* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    void* st = self->stamp;
    if (st) {
        const int32_t y = self->y + 0x17;
        ccall<void>(F_gxDrawStamp, (const void*)st, self->x, y, (int32_t)0, (const void*)0);
    }
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = G(S_COL_FE4);
        ccall<void>(F_gxRect, x, y, self->w + x, y + 0x13, col);
    }
    const int32_t y = self->y + 9;
    const int32_t x = self->x + self->w / 2;
    ccall<void>(F_UIStyleDraw, (int32_t)0x11, x, y, (const char*)self->text, (uint32_t)0);
}
static void fp_MultiTrackChooser_Draw(Footprint& f, MultiTrackChooser*, Edx, gxCanvas* c) { fp_draw(f, c); }
PORT_FN(0x0048a150, "MultiTrackChooser::Draw", MultiTrackChooser_Draw_n, fp_MultiTrackChooser_Draw)

// FIX: "XXX Race <a> of <b> XXX" (the numbers' translations) went into 36 bytes unbounded, over the count and track that
// follow; a longer one keeps its first 35 characters (fx_len above)
static void __fastcall MTC_set_race_number_n(MultiTrackChooser* self, Edx, int32_t a, int32_t b) {
    const char* sb = ccall<const char*>(F_NumberString, b);
    const char* sa = ccall<const char*>(F_NumberString, a);
    if (VP_FIX && 17 + fx_len(sa) + fx_len(sb) > sizeof self->race_text - 1) {
        char t[0x400], c1[0x100], c2[0x100];
        UI_sprintf(t, (const char*)0x004f877c, fx_cut(sa, c1, 0xff), fx_cut(sb, c2, 0xff));
        ui_copy_bounded(self->race_text, t, sizeof self->race_text);
    } else {
        UI_sprintf(self->race_text, (const char*)0x004f877c, sa, sb);        // "XXX Race %s of %s XXX"
    }
}
static void fp_MTC_set_race_number(Footprint& f, MultiTrackChooser*, Edx, int32_t, int32_t) { f.replay_only = "NumberString (its Xlators)"; }
PORT_FN(0x0048a1d0, "MultiTrackChooser::set_race_number", MTC_set_race_number_n, fp_MTC_set_race_number)

// "Number:Zero" .. "Number:Fourteen", translated
// FIX: a number outside 0..14 read the frame around the table (the return address, the argument, the callee's garbage)
// as the string -- and its callers pass numbers from the network (the proposal's AI cars, update_race) and the options
// (MULTI opponent_count, MultiRaceCfg). Such a number gives "" (its Xlators are made and refreshed as before).
static const char* __cdecl NumberString_n(int32_t n) {
    static const uint32_t keys[15] = {0x004f8794, 0x004f87a0, 0x004f87ac, 0x004f87b8, 0x004f87c8, 0x004f87d4, 0x004f87e0,
                                      0x004f87ec, 0x004f87fc, 0x004f880c, 0x004f8818, 0x004f8824, 0x004f8834, 0x004f8844,
                                      0x004f8854};
    static const uint32_t helpers[15] = {0x0048a790, 0x0048a780, 0x0048a770, 0x0048a760, 0x0048a750, 0x0048a740, 0x0048a730,
                                         0x0048a720, 0x0048a710, 0x0048a700, 0x0048a6f0, 0x0048a6e0, 0x0048a6d0, 0x0048a6c0,
                                         0x0048a6b0};
    for (int k = 0; k < 15; k++)
        once_xl(k < 8 ? S_NUM_ONCE : S_NUM_ONCE2, (uint8_t)(1u << (k < 8 ? k : k - 8)), k_number_xl[k], keys[k], helpers[k]);
    const char* volatile a[15];
    for (int k = 0; k < 15; k++) {
        xl(k_number_xl[k]);
        a[k] = UI_GP(const char, k_number_xl[k] + 4);
    }
    if (VP_FIX && (uint32_t)n > 14u) return (const char*)(uintptr_t)S_EMPTY_STR;
    return a[n];
}
static void fp_NumberString(Footprint& f, int32_t) {
    if (UI_G8(S_NUM_ONCE) != 0xff || (UI_G8(S_NUM_ONCE2) & 0x7f) != 0x7f) { f.replay_only = "constructs its Xlators (atexit)"; return; }
    for (int k = 0; k < 15; k++) f.add((void*)(uintptr_t)k_number_xl[k], 0xc, "a Number: Xlator");
}
PORT_FN(0x0048a210, "NumberString", NumberString_n, fp_NumberString)

// the track's friendly name, and " - " the reversed text when the race is reversed
// FIX: copied unbounded into 64 bytes (the set_race_number text follows); a longer text keeps its first 63 characters. And
// the track (the host's, from the network) wasn't checked against this machine's table (GetTrackFriendlyName reads past
// it); a track this machine doesn't have (as set_track) shows no name.
static void __fastcall MTC_set_text_from_race_n(MultiTrackChooser* self, Edx, const uint8_t* ri) {
    const int32_t track = *(const volatile int32_t*)(ri + NRI_TRACK);
    if (VP_FIX && (track < 0 || track >= self->count)) {
        *(volatile char*)self->text = 0;
        return;
    }
    const char* fn = ccall<const char*>(F_GetTrackFriendlyName, track);
    if (VP_FIX) {
        const char* rv = 0;
        if (*(const volatile uint8_t*)(ri + NRI_REVERSED)) {
            xl(0x00579f40);
            rv = UI_GP(const char, 0x00579f44);
        }
        if (fx_len(fn) + (rv ? 3 + fx_len(rv) : 0) > sizeof self->text - 1) {
            char t[0x210];
            ui_copy_bounded(t, fn, 0x100);
            if (rv) {
                char c1[0x100];
                crt_strcat(t, (const char*)0x004f8864);                         // " - "
                crt_strcat(t, fx_cut(rv, c1, 0xff));
            }
            ui_copy_bounded(self->text, t, sizeof self->text);
            return;
        }
    }
    crt_strcpy(self->text, fn);
    if (*(const volatile uint8_t*)(ri + NRI_REVERSED)) {
        char* e = self->text + crt_strlen(self->text);
        *(volatile uint32_t*)e = *(const volatile uint32_t*)(uintptr_t)0x004f8864;    // " - "
        xl(0x00579f40);
        crt_strcat(self->text, UI_GP(const char, 0x00579f44));
    }
}
static void fp_MTC_set_text_from_race(Footprint& f, MultiTrackChooser*, Edx, const uint8_t*) { f.replay_only = "the track's friendly name (the front end's table)"; }
PORT_FN(0x0048a7a0, "MultiTrackChooser::set_text_from_race", MTC_set_text_from_race_n, fp_MTC_set_text_from_race)

static void __fastcall MTC_set_text_from_proposal_n(MultiTrackChooser* self, Edx, const uint8_t* p) {
    tcall<void>(F_MTC_set_text_from_race, self, (const void*)(p + NP_RACE));
}
PORT_FN(0x0048a850, "MultiTrackChooser::set_text_from_proposal", MTC_set_text_from_proposal_n, fp_MTC_set_text_from_race)

static void __fastcall MTC_set_track_p_n(MultiTrackChooser* self, Edx, const uint8_t* p) {
    tcall<void>(F_MTC_set_track, self, *(const volatile int32_t*)(p + NP_TRACK));
    tcall<void>(F_MTC_set_text_from_proposal, self, (const void*)p);
}
static void fp_MTC_set_track_p(Footprint& f, MultiTrackChooser*, Edx, const uint8_t*) { f.replay_only = "loads the track's stamp"; }
PORT_FN(0x0048a860, "MultiTrackChooser::set_track(NetProposal)", MTC_set_track_p_n, fp_MTC_set_track_p)

// the track's picture: '<name>.stp'
// FIX: the name was built in 16 bytes: a name of 12 characters (the most the table held) wrote its terminator past them;
// and the index (the host's, from the network: sf_carchoice, i_running) wasn't checked against this machine's table --
// one past its tracks copied whatever followed (uninitialised in MultiDo's frame) over the frame. The name now has room
// and comes from fx_track_name (every installed track); a track this machine doesn't have shows no picture.
static void __fastcall MTC_set_track_n(MultiTrackChooser* self, Edx, int32_t t) {
    char buf[VP_FIX ? 0x110 : 0x10];
    self->track = t;
    const char* name = VP_FIX ? fx_track_name(self, t) : (const char*)self + 0x88 + 13 * t;
    if (name) {
        fx_copy(buf, name, sizeof buf - 5);
        char* e = buf + crt_strlen(buf);
        *(volatile uint32_t*)e = *(const volatile uint32_t*)(uintptr_t)0x004f8868;    // ".stp"
        *(volatile char*)(e + 4) = *(const volatile char*)(uintptr_t)0x004f886c;
    }
    void* st = self->stamp;
    if (st) ccall<void>(F_gxForgetStamp, st);
    self->stamp = name ? ccall<void*>(F_gxGetStamp, (const char*)buf) : 0;    // FIX: (above) no track: no picture
    ((void(__cdecl*)(uint32_t))(uintptr_t)UI_GU32(S_SHOWHIDE + 4))(self->grp_track);   // `call [0x4f83d4]`: UIShowGroup
    tcall<void>(F_UICC_Dirty, self);
}
static void fp_MTC_set_track(Footprint& f, MultiTrackChooser*, Edx, int32_t) { f.replay_only = "loads the track's stamp"; }
PORT_FN(0x0048a880, "MultiTrackChooser::set_track(int)", MTC_set_track_n, fp_MTC_set_track)

// the host's arrows: the next / previous track, the change sent
// FIX: no tracks (a track table of two rows or fewer: count 0) divided by 0; the arrows do nothing
static void __fastcall MTC_next_n(MultiTrackChooser* self, Edx) {
    if (VP_FIX && self->count == 0) return;
    const int32_t t = (self->track + 1) % self->count;
    self->track = t;
    self->saved = t;
    tcall<void>(F_MTC_set_track, self, t);
    tcall<void>(F_MultiMaster_config_changed, g_master());
}
static void fp_MTC_stamp_clock(Footprint& f, MultiTrackChooser*, Edx) { f.replay_only = "loads the track's stamp, reads the clock"; }
PORT_FN(0x0048a930, "MultiTrackChooser::next", MTC_next_n, fp_MTC_stamp_clock)

static void __fastcall MTC_prev_n(MultiTrackChooser* self, Edx) {
    if (VP_FIX && self->count == 0) return;                           // FIX: (next)
    const int32_t n = self->count;
    const int32_t t = (self->track + n - 1) % n;
    self->track = t;
    self->saved = t;
    tcall<void>(F_MTC_set_track, self, t);
    tcall<void>(F_MultiMaster_config_changed, g_master());
}
PORT_FN(0x0048a960, "MultiTrackChooser::prev", MTC_prev_n, fp_MTC_stamp_clock)

// Prev / Next car (with the lobby's lock let go while the car loads)
static uint8_t __cdecl MCC_PrevCar_n(int32_t) {
    if (!tcall<uint8_t>(F_LMI_AmOwner, g_master()->lmi)) net_Grab(g_master()->lmi, 0);   // site 0x48a9a9
    net_Release(g_master()->lmi, 0);                                                      // site 0x48a9b6
    tcall<void>(F_CarViewer3D_Prev, g_carch()->viewer);
    net_Grab(g_master()->lmi, 0);                                                         // site 0x48a9d1
    tcall<void>(F_MultiMaster_car_changed, g_master());
    tcall<void>(F_MCC_update_car, g_carch());
    return 0;
}
static void fp_MCC_car(Footprint& f, int32_t) { f.replay_only = "loads a car (CarViewer3D), the lobby's lock, the race server"; }
PORT_FN(0x0048a990, "MultiCarChooser::PrevCar", MCC_PrevCar_n, fp_MCC_car)

static uint8_t __cdecl MCC_NextCar_n(int32_t) {
    if (!tcall<uint8_t>(F_LMI_AmOwner, g_master()->lmi)) net_Grab(g_master()->lmi, 0);   // site 0x48aa09
    net_Release(g_master()->lmi, 0);                                                      // site 0x48aa16
    tcall<void>(F_CarViewer3D_Next, g_carch()->viewer);
    net_Grab(g_master()->lmi, 0);                                                         // site 0x48aa31
    tcall<void>(F_MultiMaster_car_changed, g_master());
    tcall<void>(F_MCC_update_car, g_carch());
    return 0;
}
PORT_FN(0x0048a9f0, "MultiCarChooser::NextCar", MCC_NextCar_n, fp_MCC_car)

// =====================================================================================================================
// MultiRaceCfg
// =====================================================================================================================
// the host's settings: the GAME section's, then the MULTI section's over them; the AI cars' text
// FIX: "<Multi:AI_Cars>: <number>" (translations) was sprintf'd into 16 bytes unbounded, over the laps' text; a longer one
// keeps its first 15 characters (fx_pair)
static MultiRaceCfg* __fastcall MultiRaceCfg_ctor_n(MultiRaceCfg* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_MultiRaceCfg;
    self->widget = 0;
    UI_GP(MultiRaceCfg, S_RACECFG_G) = self;
    for (int i = 0; i < 0xd; i++) ((volatile uint32_t*)&self->realism)[i] = 0;    // rep stosd: the NetRaceInfo
    const char* game = sec(S_SEC_MGAME);
    opt_get_i(game, 0x004f8870, &self->realism);
    opt_get_b(game, 0x004f8878, &self->damage);
    opt_get_i(game, 0x004f8884, &self->time_of_day);
    opt_get_i(game, 0x004f8890, &self->weather);
    opt_get_i(game, 0x004f8898, &self->race_type);
    const char* multi = sec(S_SEC_MULTI);
    opt_get_i(multi, 0x004f88a4, &self->realism);
    opt_get_b(multi, 0x004f88ac, &self->damage);
    opt_get_i(multi, 0x004f88b8, &self->time_of_day);
    opt_get_i(multi, 0x004f88c4, &self->weather);
    opt_get_i(multi, 0x004f88cc, &self->race_type);
    opt_get_i(multi, 0x004f88d8, &self->strength);
    opt_get_b(multi, 0x004f88ec, &self->iroc);
    opt_get_b(multi, 0x004f88f4, &self->reversed);
    self->i24 = 3;
    volatile int32_t count = 0;
    opt_get_i(multi, 0x004f8900, &count);                                     // "opponent_count"
    self->opp_f = (float)count;
    self->type_f = (float)self->race_type;
    xl(0x0057a020);
    const char* num = ccall<const char*>(F_NumberString, x87_ftol(D(self->opp_f)));
    fx_pair(self->opp_text, sizeof self->opp_text, 0x004f8910, UI_GP(const char, 0x0057a024), num);    // "%s: %s" (FIX: fx_pair)
    return self;
}
static void fp_MultiRaceCfg_ctor(Footprint& f, MultiRaceCfg*, Edx) { f.replay_only = "reads the options"; }
PORT_FN(0x0048aa50, "MultiRaceCfg::MultiRaceCfg", MultiRaceCfg_ctor_n, fp_MultiRaceCfg_ctor)

static void __fastcall MultiRaceCfg_dtor_n(MultiRaceCfg* self, Edx) {
    const uint8_t rev = self->reversed;
    self->vtbl = (const void*)(uintptr_t)VT_MultiRaceCfg;
    const char* multi = sec(S_SEC_MULTI);
    opt_set_b(multi, 0x004f8918, rev);
    opt_set_i(multi, 0x004f8924, self->realism);
    opt_set_b(multi, 0x004f892c, self->damage);
    opt_set_i(multi, 0x004f8938, self->time_of_day);
    opt_set_i(multi, 0x004f8944, self->weather);
    opt_set_i(multi, 0x004f894c, self->race_type);
    opt_set_i(multi, 0x004f8958, self->strength);
    opt_set_i(multi, 0x004f896c, x87_ftol(D(self->opp_f)));
    opt_set_b(multi, 0x004f897c, self->iroc);
    UI_GP(MultiRaceCfg, S_RACECFG_G) = 0;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_MultiRaceCfg_dtor(Footprint& f, MultiRaceCfg*, Edx) { f.replay_only = "writes the options"; }
PORT_FN(0x0048abf0, "MultiRaceCfg::~MultiRaceCfg", MultiRaceCfg_dtor_n, fp_MultiRaceCfg_dtor)

// the settings watched: the race info (id 0), the slaved cars (0), the AI cars (1), the race type (2); each told once
static void __fastcall MultiRaceCfg_Create_n(MultiRaceCfg* self, Edx) {
    ccall<int32_t>(F_PTimeNow);                                                // (its result unused)
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->realism, (uint32_t)0x34);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->iroc, (uint32_t)1);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)1, (const void*)&self->opp_f, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)2, (const void*)&self->type_f, (uint32_t)4);
    typedef void(__fastcall * Cb)(void*, Edx, int32_t, const void*);
    const Cb cb = (Cb)(*(void* const* volatile*)self)[0x10 / 4];             // read once, called three times
    cb(self, 0, 0, (const void*)&self->iroc);
    cb(self, 0, 1, (const void*)&self->opp_f);
    cb(self, 0, 2, (const void*)&self->type_f);
}
static void fp_MultiRaceCfg_Create(Footprint& f, MultiRaceCfg*, Edx) { f.replay_only = "adds notifications, reads the clock"; }
PORT_FN(0x0048acc0, "MultiRaceCfg::Create", MultiRaceCfg_Create_n, fp_MultiRaceCfg_Create)

static void __fastcall MultiRaceCfg_Destroy_n(MultiRaceCfg* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->iroc);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->opp_f);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->type_f);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->realism);
}
PORT_FN(0x0048ad30, "MultiRaceCfg::Destroy", MultiRaceCfg_Destroy_n, fp_notes0)

// 0: the change sent (config_changed); 1, 4: the AI cars from their slider (the strength's group shown with any), their
// text; 2: the race type from its slider, its laps on this track, their text; else a panic
// FIX: "<GameOptions:Laps>: <laps>" was sprintf'd into 24 bytes unbounded (the groups follow), and the AI cars' text into
// 16 (as the constructor's); each keeps what fits. The track's name for its laps comes from fx_track_name (a track past the
// chooser's first 16 read past its table); a track this machine doesn't have leaves the laps as they were.
static void __fastcall MultiRaceCfg_Callback_n(MultiRaceCfg* self, Edx, int32_t id, const void*) {
    once_xl(S_RACECFG_ONCE, 1, 0x0057a1f8, 0x004f8984, 0x0048aef0);           // "Multi:AI_Cars"
    once_xl(S_RACECFG_ONCE, 2, 0x0057a238, 0x004f8994, 0x0048aee0);           // "GameOptions:Laps"
    switch ((uint32_t)id) {
    case 0:
        tcall<void>(F_MultiMaster_config_changed, g_master());
        return;
    case 1:
    case 4: {
        const int32_t n = x87_ftol(D(self->opp_f));
        self->opponents = n;
        const uint32_t g = self->grp_opp;
        if (n == 0) hide(g);
        else show(g);
        xl(0x0057a1f8);
        const char* num = ccall<const char*>(F_NumberString, x87_ftol(D(self->opp_f)));
        fx_pair(self->opp_text, sizeof self->opp_text, 0x004f89a8, UI_GP(const char, 0x0057a1fc), num);   // "%s: %s" (FIX)
        return;
    }
    case 2: {
        const int32_t t = x87_ftol(D(self->type_f));
        self->race_type = t;
        const int32_t idx = g_track()->track;
        const char* name = VP_FIX ? fx_track_name(g_track(), idx) : (const char*)g_track() + 13 * idx + 0x88;   // FIX: (above)
        if (name) self->laps = ccall<int32_t>(F_GetLapCountFromType, t, name);
        xl(0x0057a238);
        const char* lt = UI_GP(const char, 0x0057a23c);
        if (VP_FIX && fx_len(lt) + 2 + fx_dec(self->laps) > sizeof self->laps_text - 1) {   // FIX: (above)
            char tb[0x400], c1[0x100];
            UI_sprintf(tb, (const char*)0x004f89b0, fx_cut(lt, c1, 0xff), self->laps);
            ui_copy_bounded(self->laps_text, tb, sizeof self->laps_text);
        } else {
            UI_sprintf(self->laps_text, (const char*)0x004f89b0, lt, self->laps);   // "%s: %d"
        }
        return;
    }
    default:
        UI_LogPanic((const char*)0x004f89b8, 0x745);                         // "(msched.cpp:%d) Whatever."
        return;
    }
}
static void fp_MultiRaceCfg_Callback(Footprint& f, MultiRaceCfg* self, Edx, int32_t id, const void*) {
    if ((UI_G8(S_RACECFG_ONCE) & 3) != 3) { f.replay_only = "constructs its Xlators (atexit)"; return; }
    if (id == 0) { f.replay_only = "config_changed reads the clock (PTimeNow)"; return; }
    if (id == 2) { f.replay_only = "the laps of the track (the front end's table)"; return; }
    if (id != 1 && id != 4) { f.replay_only = "LogPanic"; return; }
    if (UI_G8(S_NUM_ONCE) != 0xff || (UI_G8(S_NUM_ONCE2) & 0x7f) != 0x7f) { f.replay_only = "NumberString constructs its Xlators (atexit)"; return; }
    f.add(self, sizeof(MultiRaceCfg), "this");
    f.add((void*)(uintptr_t)0x0057a1f8, 0xc, "Xlator Multi:AI_Cars");
    for (int k = 0; k < 15; k++) f.add((void*)(uintptr_t)k_number_xl[k], 0xc, "a Number: Xlator");
    fp_groups(f);
}
PORT_FN(0x0048ad60, "MultiRaceCfg::Callback", MultiRaceCfg_Callback_n, fp_MultiRaceCfg_Callback)

// the proposal from the settings: the slaved cars, the race info, the AI cars, the strength
static void __fastcall MRC_fillout_n(MultiRaceCfg* self, Edx, uint8_t* p) {
    const uint8_t iroc = self->iroc;
    *(volatile uint8_t*)(p + NP_IROC) = iroc;
    crt_copy(p + NP_RACE, (const void*)&self->realism, 0x28);
    *(volatile int32_t*)(p + 0x34) = self->opponents;
    *(volatile int32_t*)(p + 0x28) = self->strength;
}
static void fp_MRC_fillout(Footprint& f, MultiRaceCfg*, Edx, uint8_t* p) { f.add(p, 0x3c, "the proposal"); }
PORT_FN(0x0048af00, "MultiRaceCfg::fillout", MRC_fillout_n, fp_MRC_fillout)

// the settings' widgets: realism (three radio buttons), damage, reversed, the slaved cars, the AI cars (a slider, its text),
// their strength (three), the laps' text and the race type (a slider)
static void __fastcall MultiRaceCfg_Added_n(MultiRaceCfg* self, Edx) {
    Frame<0x498> fr;
    IC(0x498, 1, 0, 0, 0, 0, 0, 0, 0, S(0x84), 0, 0, 0, 0, 0);
    {
        const char* s = ccall<const char*>(F_GetRealismString, (int32_t)0);
        IC(0x460, 0xc, 0, 0xf5, 0x5a, 0, 0, U(s), 0, S(0x18), 8, 0, 0, 0, 0);
    }
    {
        const char* s = ccall<const char*>(F_GetRealismString, (int32_t)1);
        IC(0x428, 0xc, 0, 0xf5, 0x69, 0, 0, U(s), 1, S(0x18), 8, 0, 0, 0, 0);
    }
    {
        const char* s = ccall<const char*>(F_GetRealismString, (int32_t)2);
        IC(0x3f0, 0xc, 0, 0xf5, 0x78, 0, 0, U(s), 2, S(0x18), 8, 0, 0, 0, 0);
    }
    xl(0x00579f30);
    IC(0x3b8, 0xb, 0, 0xf5, 0x87, 0, 0, G(0x00579f34), 0, S(0x3c), 7, 0, 0, 0, 0);
    xl(0x00579f40);
    IC(0x380, 0xb, 0, 0xf5, 0xa0, 0, 0, G(0x00579f44), 0, S(0x3d), 7, 0, 0, 0, 0);
    xl(0x0057a010);
    IC(0x348, 0xb, 0, 0xf5, 0xaf, 0, 0, G(0x0057a014), 0, S(0x4c), 7, 0, 0, 0, 0);
    IC(0x310, 5, 0, 0x1c2, 0x5a, 0, 0, S(0x58), 0, 0, sty1(S_COL_1D4), 0, 0, 0, 0);
    IC(0x2d8, 0xf, 0, 0x1d6, 0x69, 0x64, 8, S_EMPTY_STR, (uint32_t)-7, S(0x50), 0, 0, 0x40c00000u, 0, 0);   // 0 .. 6.0
    IC(0x2a0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x80), 0, 0, 0, 0, 0);
    xl(0x0057a248);
    {
        const uint32_t st = sty1(S_COL_1D4);
        IC(0x268, 5, 0, 0x1c2, 0x82, 0, 0, G(0x0057a24c), 0, 0, st, 0, 0, 0, 0);
    }
    {
        const char* s = ccall<const char*>(F_GetAIStrengthString, (int32_t)0);
        IC(0x230, 0xc, 0, 0x1cc, 0x96, 0, 0, U(s), 0, S(0x38), 8, 0, 0, 0, 0);
    }
    {
        const char* s = ccall<const char*>(F_GetAIStrengthString, (int32_t)1);
        IC(0x1f8, 0xc, 0, 0x1cc, 0xa5, 0, 0, U(s), 1, S(0x38), 8, 0, 0, 0, 0);
    }
    {
        const char* s = ccall<const char*>(F_GetAIStrengthString, (int32_t)2);
        IC(0x1c0, 0xc, 0, 0x1cc, 0xb4, 0, 0, U(s), 2, S(0x38), 8, 0, 0, 0, 0);
    }
    IC(0x188, 1, 1, 0, 0, 0, 0, 0, 0, S(0x80), 0, 0, 0, 0, 0);
    IC(0x150, 1, 0, 0, 0, 0, 0, 0, 0, S(0x88), 0, 0, 0, 0, 0);
    IC(0x118, 5, 0, 0xf5, 0xc8, 0, 0, S(0x68), 0, 0, sty1(S_COL_1D4), 0, 0, 0, 0);
    ccall<void*>(F_UI_SLIDER, fr.at(0xe0), (int32_t)0x109, (int32_t)0xd7, (int32_t)0x64, (int32_t)8, (void*)&self->type_f,
                 (uint32_t)0x3f800000u, (uint32_t)0x40400000u, (int32_t)3);                 // 1.0 .. 3.0, 3 steps
    IC(0xa8, 1, 1, 0, 0, 0, 0, 0, 0, S(0x88), 0, 0, 0, 0, 0);
    IC(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0x84), 0, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x498));
}
PORT_FN(0x0048af30, "MultiRaceCfg::Added", MultiRaceCfg_Added_n, fp_items0)

// =====================================================================================================================
// MultiRaceInfo
// =====================================================================================================================
static MultiRaceInfo* __fastcall MultiRaceInfo_ctor_n(MultiRaceInfo* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_MultiRaceInfo;
    self->gc_type = 1;
    self->widget = 0;
    opt_get_i(sec(S_SEC_MULTI), 0x004f89d4, &self->gc_type);                 // "gc_type"
    UI_GP(MultiRaceInfo, S_RACEINFO_G) = self;
    for (int i = 0; i < 0x1e; i++) ((volatile uint32_t*)self->realism)[i] = 0;    // rep stosd: the texts
    *(volatile char*)self->title = 0;
    return self;
}
static void fp_MultiRaceInfo_ctor(Footprint& f, MultiRaceInfo*, Edx) { f.replay_only = "reads the options"; }
PORT_FN(0x0048b3c0, "MultiRaceInfo::MultiRaceInfo", MultiRaceInfo_ctor_n, fp_MultiRaceInfo_ctor)

static void __fastcall MultiRaceInfo_dtor_n(MultiRaceInfo* self, Edx) {
    const int32_t gc = self->gc_type;
    self->vtbl = (const void*)(uintptr_t)VT_MultiRaceInfo;
    opt_set_i(sec(S_SEC_MULTI), 0x004f89dc, gc);                             // "gc_type"
    UI_GP(MultiRaceInfo, S_RACEINFO_G) = 0;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_MultiRaceInfo_dtor(Footprint& f, MultiRaceInfo*, Edx) { f.replay_only = "writes the options"; }
PORT_FN(0x0048b420, "MultiRaceInfo::~MultiRaceInfo", MultiRaceInfo_dtor_n, fp_MultiRaceInfo_dtor)

static void __fastcall MultiRaceInfo_Create_n(MultiRaceInfo* self, Edx) {
    hide(self->grp_all);
    hide(self->grp_info);
}
static void fp_MultiRaceInfo_Create(Footprint& f, MultiRaceInfo*, Edx) { fp_groups(f); }
PORT_FN(0x0048b460, "MultiRaceInfo::Create", MultiRaceInfo_Create_n, fp_MultiRaceInfo_Create)

// the proposal shown: "Multi:Enabled" over it when the cars are slaved
// FIX: the texts (translations, the front end's names) were copied unbounded into 16 bytes each (24 for the race type's
// "%s: %d %s"), each running into the next and the last into the groups; each keeps what fits (15, 23 characters)
static void __cdecl MRI_UpdateProposal_n(const uint8_t* p) {
    const char* s;
    if (*(const volatile uint8_t*)(p + NP_IROC)) {
        xl(0x0057a010);
        s = UI_GP(const char, 0x0057a014);
    } else {
        s = (const char*)0x004f89e4;                                          // ""
    }
    fx_copy(g_info()->title, s, sizeof g_info()->title);                      // FIX: (above)
    tcall<void>(F_MRI_update_race, g_info(), (const void*)(p + NP_RACE), (uint32_t)*(const volatile uint8_t*)(p + NP_IROC));
}
static void fp_MRI_UpdateProposal(Footprint& f, const uint8_t*) { f.replay_only = "the front end's strings, NumberString (its Xlators)"; }
PORT_FN(0x0048b490, "MultiRaceInfo::UpdateProposal", MRI_UpdateProposal_n, fp_MRI_UpdateProposal)

static void __fastcall MRI_update_race_n(MultiRaceInfo* self, Edx, const uint8_t* ri, uint8_t iroc) {
    show(self->grp_info);
    once_xl(S_RACE_ONCE, 1, 0x0057a0d0, 0x004f89e8, 0x0048b820);              // "Multi:Enabled"
    once_xl(S_RACE_ONCE, 2, 0x0057a180, 0x004f89f8, 0x0048b810);              // "Multi:Disabled"
    once_xl(S_RACE_ONCE, 4, 0x0057a108, 0x004f8a08, 0x0048b800);              // "GameOptions:Laps"
    once_xl(S_RACE_ONCE, 8, 0x0057a038, 0x004f8a1c, 0x0048b7f0);              // "GameOptions:Lap"
    const char* s;
    if (iroc) {
        xl(0x0057a010);
        s = UI_GP(const char, 0x0057a014);
    } else {
        s = (const char*)0x004f8a2c;                                          // ""
    }
    fx_copy(self->title, s, sizeof self->title);                             // FIX: (UpdateProposal) each held to its field
    fx_copy(self->realism, ccall<const char*>(F_GetRealismString, *(const volatile int32_t*)(ri + NRI_REALISM)), sizeof self->realism);
    if (*(const volatile uint8_t*)(ri + NRI_DAMAGE)) {
        xl(0x0057a0d0);
        s = UI_GP(const char, 0x0057a0d4);
    } else {
        xl(0x0057a180);
        s = UI_GP(const char, 0x0057a184);
    }
    fx_copy(self->damage, s, sizeof self->damage);
    fx_copy(self->time, ccall<const char*>(F_GetGameTimeString, *(const volatile int32_t*)(ri + NRI_TIME)), sizeof self->time);
    fx_copy(self->weather, ccall<const char*>(F_GetWeatherString, *(const volatile int32_t*)(ri + NRI_WEATHER)), sizeof self->weather);
    uint32_t lap;
    if (*(const volatile int32_t*)(ri + NRI_LAPS) == 1) {
        xl(0x0057a038);
        lap = G(0x0057a03c);
    } else {
        xl(0x0057a108);
        lap = G(0x0057a10c);
    }
    {
        const int32_t type = *(const volatile int32_t*)(ri + NRI_TYPE);
        const int32_t laps = *(const volatile int32_t*)(ri + NRI_LAPS);
        const char* ts = ccall<const char*>(F_GetRaceTypeString, type);
        const char* ls = (const char*)(uintptr_t)lap;
        if (VP_FIX && fx_len(ts) + 2 + fx_dec(laps) + 1 + fx_len(ls) > sizeof self->type - 1) {   // FIX: (UpdateProposal)
            char t[0x400], c1[0x100], c2[0x100];
            UI_sprintf(t, (const char*)0x004f8a30, fx_cut(ts, c1, 0xff), laps, fx_cut(ls, c2, 0xff));
            ui_copy_bounded(self->type, t, sizeof self->type);
        } else {
            UI_sprintf(self->type, (const char*)0x004f8a30, ts, laps, ls);    // "%s: %d %s"
        }
    }
    fx_copy(self->count, ccall<const char*>(F_NumberString, *(const volatile int32_t*)(ri + NRI_COUNT)), sizeof self->count);
    const uint32_t g = self->grp_opp;
    if (*(const volatile int32_t*)(ri + NRI_COUNT) != 0) {
        show(g);
        fx_copy(self->strength, ccall<const char*>(F_GetAIStrengthString, *(const volatile int32_t*)(ri + NRI_STRENGTH)),
                sizeof self->strength);
    } else {
        hide(g);
    }
}
static void fp_MRI_update_race(Footprint& f, MultiRaceInfo*, Edx, const uint8_t*, uint8_t) { f.replay_only = "the front end's strings, NumberString (its Xlators)"; }
PORT_FN(0x0048b500, "MultiRaceInfo::update_race", MRI_update_race_n, fp_MRI_update_race)

// the proposal's texts in their groups; the ghost car's type (a Multi of three) and the Options button
// FIX: the ghost types' three translations are joined (CreateMultiString: each with its terminator, and one more) into 64
// bytes at 0x57a048: longer ones ran over the lost-connection box's title Xlator at 0x57a088 (its key, then its text,
// became string bytes). When they'd pass the 64 bytes, each is cut to 20 characters (3 x 21 + 1 = 64).
static void __fastcall MultiRaceInfo_Added_n(MultiRaceInfo* self, Edx) {
    Frame<0x4ac> fr;
    once_xl(S_RACEINFO_ONCE, 1, 0x0057a098, 0x004f8a3c, 0x0048be50);          // "GameOptions:GhostType:Brief:Off"
    once_xl(S_RACEINFO_ONCE, 2, 0x0057a0e0, 0x004f8a5c, 0x0048be40);          // "...:Normal"
    once_xl(S_RACEINFO_ONCE, 4, 0x0057a148, 0x004f8a80, 0x0048be30);          // "...:BestEver"
    once_xl(S_RACEINFO_ONCE, 8, 0x00579f98, 0x004f8aa8, 0x0048be20);          // "GameOptions:GhostCarType"
    once_xl(S_RACEINFO_ONCE, 0x10, 0x0057a128, 0x004f8ac4, 0x0048be10);       // "Main_Menu:Options"
    xl(0x0057a148);
    xl(0x0057a0e0);
    xl(0x0057a098);
    {
        uint32_t c = G(0x0057a14c), b = G(0x0057a0e4), a = G(0x0057a09c);
        char ca[0x15], cb[0x15], cc[0x15];
        if (VP_FIX && fx_len((const char*)(uintptr_t)a) + fx_len((const char*)(uintptr_t)b) + fx_len((const char*)(uintptr_t)c) + 4 > 0x40) {
            a = U(fx_cut((const char*)(uintptr_t)a, ca, 0x14));
            b = U(fx_cut((const char*)(uintptr_t)b, cb, 0x14));
            c = U(fx_cut((const char*)(uintptr_t)c, cc, 0x14));
        }
        ccall<void>(F_CreateMultiString, (char*)(uintptr_t)S_GHOST_MULTI, a, b, c, (uint32_t)0);
    }
    xl(0x0057a0f8);
    const uint32_t t0fc = G(0x0057a0fc);
    xl(0x00579f30);
    fr.d(0x4a4) = G(0x00579f34);
    xl(0x0057a1e8);
    const uint32_t t1ec = G(0x0057a1ec);
    xl(0x0057a020);
    fr.d(0x4a0) = G(0x0057a024);
    xl(0x0057a248);
    fr.d(0x49c) = G(0x0057a24c);
    IC(0x498, 1, 0, 0, 0, 0, 0, 0, 0, S(0xa4), 0, 0, 0, 0, 0);
    IC(0x460, 1, 0, 0, 0, 0, 0, 0, 0, S(0xa0), 0, 0, 0, 0, 0);
    IC(0x428, 5, 0, 0xf2, 0x57, 0, 0, t1ec, 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x3f0, 5, 0, 0x102, 0x61, 0, 0, S(0x68), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x3b8, 1, 1, 0, 0, 0, 0, 0, 0, S(0xa0), 0, 0, 0, 0, 0);
    IC(0x380, 5, 0, 0xf2, 0x6f, 0, 0, t0fc, 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x348, 5, 0, 0x102, 0x79, 0, 0, S(0x28), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x310, 5, 0, 0xf2, 0x87, 0, 0, fr.d(0x4a4), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x2d8, 5, 0, 0x102, 0x91, 0, 0, S(0x38), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x2a0, 5, 0, 0xf2, 0x9f, 0, 0, fr.d(0x4a0), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x268, 5, 0, 0x102, 0xa9, 0, 0, S(0x80), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x230, 1, 0, 0, 0, 0, 0, 0, 0, S(0xa8), 0, 0, 0, 0, 0);
    IC(0x1f8, 5, 0, 0xf2, 0xb7, 0, 0, fr.d(0x49c), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x1c0, 5, 0, 0x102, 0xc1, 0, 0, S(0x90), 0, 0, 0x12, 0, 0, 0, 0);
    IC(0x188, 1, 1, 0, 0, 0, 0, 0, 0, S(0xa8), 0, 0, 0, 0, 0);
    RAW(0x150, 5, 0, 0xf2, 0xcf, 0, 0, S(0x18), 0, 0, 0xb, 0, 0, 0, 0);
    xl(0x0057a128);
    IC(0x118, 2, 0, 0xc8, 0x1ae, 0, 0, G(0x0057a12c), 0, F_options_cb, 4, 0, 0, 0, 0);
    xl(0x00579f98);
    IC(0xe0, 5, 0, 0xf2, 0x104, 0, 0, G(0x00579f9c), 0, 0, 0xc, 0, 0, 0, 0);
    RAW(0xa8, 8, 0, 0x160, 0x104, 0, 0, S_GHOST_MULTI, 0, S(0xac), 0x15, 0, 0, 0, 0);
    IC(0x70, 1, 1, 0, 0, 0, 0, 0, 0, S(0xa4), 0, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x498));
}
PORT_FN(0x0048b830, "MultiRaceInfo::Added", MultiRaceInfo_Added_n, fp_items0)

// =====================================================================================================================
// the scheduler
// =====================================================================================================================
// the post-race box's idle function: the network ticked
static uint8_t __cdecl PostRace_Tick_n(int32_t*) {
    tcall<void>(F_SM_Tick, lmi_ptr(UI_GP(void, S_POSTRACE_LMI), LMI_SM));
    vcall<void>(lmi_ptr(UI_GP(void, S_POSTRACE_LMI), LMI_CLIENT), C_TICK);
    void* srv = lmi_ptr(UI_GP(void, S_POSTRACE_LMI), LMI_SERVER);
    if (srv) vcall<void>(srv, S_TICK);
    return 0;
}
static void fp_PostRace_Tick(Footprint& f, int32_t*) { f.replay_only = "ticks the session, the race client and server (the multi library)"; }
PORT_FN(0x0048be60, "PostRace::Tick", PostRace_Tick_n, fp_PostRace_Tick)

// the live game made (the first time): the session on the Multiplayer screen's socket, the host's race server and the
// client on it, or a client connecting to the host found; then the lobby (MultiDo)
// FIX: the game's name was copied unbounded into LiveMultiInfo's 32 bytes at +0x10, over its task id (a service's name off
// the wire needn't end inside its 32 bytes); it keeps 31 characters
static uint8_t __cdecl MenuMultiScheduler_n(uint8_t* lmi, const uint8_t* gi) {
    if (*(volatile uint8_t*)(lmi + LMI_LIVE) == 0) {
        void* volatile* const smp = (void* volatile*)(lmi + LMI_SM);
        void* sock = *(void* const volatile*)(gi + MGI_SOCKET);
        if (*(const volatile uint8_t*)(gi + MGI_HOST))
            *smp = ccall<void*>(F_CreateSessionMgr, sock, (int32_t)9, (int32_t)1, (int32_t)1);
        else
            *smp = ccall<void*>(F_CreateSessionMgr, sock, (int32_t)1, (int32_t)1, (int32_t)0);
        tcall<void>(F_SM_SetConnectPassword, *smp, (const char*)(gi + MGI_PASSWORD));
        void* sm = *smp;
        if (sm) {
            if (*(const volatile uint8_t*)(gi + MGI_HOST)) {
                void* srv = ccall<void*>(F_create_server, sm, gi);
                *(void* volatile*)(lmi + LMI_SERVER) = srv;
                if (srv) {
                    const uint32_t id = vcall<uint32_t>(srv, S_SERVICE_ID);
                    *(void* volatile*)(lmi + LMI_CLIENT) = ccall<void*>(F_CreateRaceClient_id, *smp, id);
                }
            } else {
                *(void* volatile*)(lmi + LMI_CLIENT) = ccall<void*>(F_create_client, gi, sm);
            }
        }
        fx_copy((char*)(lmi + LMI_NAME), (const char*)(gi + MGI_NAME), 0x20);    // FIX: (above)
        *(volatile uint8_t*)(lmi + LMI_LIVE) = *(void* const volatile*)(lmi + LMI_CLIENT) ? 1 : 0;
    } else {
        ccall<void>(F_MultiPostRaceDo, lmi);
    }
    if (*(volatile uint8_t*)(lmi + LMI_LIVE)) return ccall<uint8_t>(F_MultiDo, lmi);
    return 0;
}
static void fp_MenuMultiScheduler(Footprint& f, uint8_t*, const uint8_t*) { f.replay_only = "makes the session, the race server and client, runs the lobby (modal)"; }
PORT_FN(0x0048be90, "MenuMultiScheduler", MenuMultiScheduler_n, fp_MenuMultiScheduler)

// the lobby: its frame holds the controls (as the original's: 0x6fc the LiveMultiKiller, 0x6e0 the items, 0x710 the
// UIDialog, 0x4e8 the MultiMaster, 0x450 the MultiRaceInfo, 0x3a0 the ChatControl, 0x2cc the MultiTrackChooser, 0x160 the
// MultiCarChooser); the dialog runs till the master's done (Idle) or Back; the result: the client is getting its car
// (the race follows)
// FIX: (MultiMaster::Update) when the lobby saw car get and the client is already past it (a state of the race, 0xe ..
// 0x15) -- the case Update's fix now ends the lobby for, where the original crashed -- the race follows as from car get.
// (The original's lobby can't otherwise end with its master done and the state past 13.)
static uint8_t __cdecl MultiDo_n(uint8_t* lmi) {
    Frame<0x710> fr;
    fr.d(0x6e8) = 0;                                                         // the LiveMultiKiller: its widget
    fr.d(0x6fc) = VT_LiveMultiKiller;                                        // ... vtable
    fr.d(0x6e4) = U(lmi);                                                    // ... lmi
    const uint8_t host = *(void* const volatile*)(lmi + LMI_SERVER) ? 1 : 0;
    if (!ccall<uint8_t>(F_MultiEnabled)) {
        net_Grab(lmi, 0);                                                    // site 0x48bf98
        net_Release(lmi, 0);                                                 // site 0x48bf9f
    }
    tcall<void*>(F_ChatControl_ctor, fr.at(0x3a0), lmi_ptr(lmi, LMI_CLIENT));
    tcall<void*>(F_MultiTrackChooser_ctor, fr.at(0x2cc), (uint32_t)host);
    tcall<void*>(F_MultiRaceInfo_ctor, fr.at(0x450));
    tcall<void*>(F_MultiCarChooser_ctor, fr.at(0x160));
    tcall<void*>(F_MultiMaster_ctor, fr.at(0x4e8), (void*)lmi);
    once_xl(S_MULTIDO_ONCE, 1, 0x00579f20, 0x004f8ad8, 0x0048c370);           // "UI:Back"
    once_xl(S_MULTIDO_ONCE, 2, 0x0057a1d8, 0x004f8ae0, 0x0048c360);           // "Titles:MultiPlayer"
    IC(0x6e0, 0x17, 0, 0, 0, 0, 0, S_EMPTY_STR, 0, U(fr.at(0x6fc)), 0, 0, 0, 0, 0);
    IC(0x6a8, 0x17, 0, 0x1fa, 0x136, 0x64, 0x58, S_EMPTY_STR, 0, U(fr.at(0x3a0)), 0, 0, 0, 0, 0);
    RAW(0x670, 0x17, 0, 0x19, 0x52, 0xb3, 0x91, S_EMPTY_STR, 0, U(fr.at(0x2cc)), 0, 0, 0, 0, 0);
    IC(0x638, 0x17, 0, 0, 0, 0, 0, S_EMPTY_STR, 0, U(fr.at(0x4e8)), 0, 0, 0, 0, 0);
    RAW(0x600, 0x17, 0, 0, 0, 0, 0, S_EMPTY_STR, 0, U(fr.at(0x450)), 0, 0, 0, 0, 0);
    IC(0x5c8, 0x17, 0, 0, 0, 0, 0, S_EMPTY_STR, 0, U(fr.at(0x160)), 0, 0, 0, 0, 0);
    IC(0x590, 4, 0, 0x1b, 0, 0, 0, 0, 0, F_MultiMaster_ExitButton, 0, 0, 0, 0, 0);      // Escape
    xl(0x00579f20);
    RAW(0x558, 2, 0, 0x13, 0x1a2, 0, 0, G(0x00579f24), 0, F_MultiMaster_ExitButton, 6, 0, 0, 0, 0);
    item_end(fr.at(0x520));
    xl(0x0057a1d8);
    fr.d(0x710) = G(0x0057a1dc);                                             // UIDialog: the title
    fr.d(0x704) = U(fr.at(0x6e0));                                           // ... the items
    fr.d(0x70c) = 0x004f8af4;                                                // ... "multi.stp"
    fr.d(0x708) = 0xfffffffeu;                                               // ... the default (-2)
    fr.d(0x700) = F_MultiMaster_Idle;                                        // ... the idle function
    ccall<void>(F_ResourceSetMustLoad, (const char*)0x004f8b00);            // "postrace.res"
    ccall<int32_t>(F_UIDoDialog, fr.at(0x710), UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), (int32_t)-999, (int32_t)-999, (int32_t)1);
    ccall<void>(F_ResourceSetUnload, (const char*)0x004f8b10);
    const int32_t st = client_state(lmi_ptr(lmi, LMI_CLIENT));
    uint8_t r = st == 0xd ? 1 : 0;
    if (VP_FIX && st > 0xd && st <= 0x15 && ((MultiMaster*)fr.at(0x4e8))->done) r = 1;   // FIX: (above)
    tcall<void>(F_MultiMaster_dtor, fr.at(0x4e8));
    tcall<void>(F_MultiCarChooser_dtor, fr.at(0x160));
    tcall<void>(F_MultiRaceInfo_dtor, fr.at(0x450));
    tcall<void>(F_MultiTrackChooser_dtor, fr.at(0x2cc));
    tcall<void>(F_ChatControl_dtor, fr.at(0x3a0));
    return r;
}
static void fp_MultiDo(Footprint& f, uint8_t*) { f.replay_only = "the lobby: runs a dialog (modal), builds and destroys its controls"; }
PORT_FN(0x0048bf60, "MultiDo", MultiDo_n, fp_MultiDo)

static void __cdecl MultiPostRaceDo_n(void*) {}
static void fp_MultiPostRaceDo(Footprint& f, void*) { f.pure = true; }
PORT_FN(0x0048c380, "MultiPostRaceDo", MultiPostRaceDo_n, fp_MultiPostRaceDo)

// the host's server: its name, and the password (or none, when it's empty)
static void* __cdecl create_server_n(void* sm, const uint8_t* gi) {
    const char* pw = (const char*)(gi + MGI_PASSWORD);
    const char* p = *(const volatile uint8_t*)pw ? pw : 0;
    return ccall<void*>(F_CreateRaceServer, sm, (const char*)(gi + MGI_NAME), p);
}
static void fp_create_server(Footprint& f, void*, const uint8_t*) { f.replay_only = "makes the race server (the multi library)"; }
PORT_FN(0x0048c390, "create_server", create_server_n, fp_create_server)

// the client: connecting by the host's name, or to the service the Multiplayer screen found
static void* __cdecl create_client_n(const uint8_t* gi, void* sm) {
    if (*(const volatile uint8_t*)(gi + MGI_BY_NAME)) return ccall<void*>(F_CreateRaceClient_name, sm, (const char*)(gi + MGI_NAME));
    return ccall<void*>(F_CreateRaceClient_rs, sm, (const void*)(gi + MGI_NAME));
}
static void fp_create_client(Footprint& f, const uint8_t*, void*) { f.replay_only = "makes the race client (the multi library)"; }
PORT_FN(0x0048c3c0, "create_client", create_client_n, fp_create_client)

// =====================================================================================================================
// the small ones: mouse handlers, deleting destructors, the static callbacks, the empty Draws, UI_SLIDER, LiveMultiKiller
// =====================================================================================================================
static void __fastcall ChatControl_MouseDown_n(ChatControl* self, Edx, int32_t x, int32_t y) {
    self->dragging = 1;
    vcall<void>(self, 0x28, x, y);                                           // MouseMove
}
static void fp_ChatControl_MouseDown(Footprint& f, ChatControl*, Edx, int32_t, int32_t) { f.replay_only = "MouseMove asks the race client (the multi library)"; }
PORT_FN(0x0048c3f0, "ChatControl::MouseDown", ChatControl_MouseDown_n, fp_ChatControl_MouseDown)

static void __fastcall ChatControl_MouseUp_n(ChatControl* self, Edx, int32_t, int32_t) { self->dragging = 0; }
static void fp_ChatControl_MouseUp(Footprint& f, ChatControl* self, Edx, int32_t, int32_t) { f.add(self, sizeof(ChatControl), "this"); }
PORT_FN(0x0048c410, "ChatControl::MouseUp", ChatControl_MouseUp_n, fp_ChatControl_MouseUp)

static void* __fastcall ChatControl_sdtor_n(ChatControl* self, Edx, uint32_t flags) {
    tcall<void>(F_ChatControl_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c420, "ChatControl::scalar deleting destructor", ChatControl_sdtor_n, fp_frees)

static uint8_t __cdecl ChatControl_Speak_n(int32_t) { return tcall<uint8_t>(F_ChatControl_speak, UI_GP(ChatControl, S_CHAT_G)); }
PORT_FN(0x0048c440, "ChatControl::Speak", ChatControl_Speak_n, fp_cb_client)

static void* __fastcall ChatWindow_vdtor_n(ChatWindow* self, Edx, uint32_t flags) {
    tcall<void>(F_ChatWindow_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c450, "ChatWindow::vector deleting destructor", ChatWindow_vdtor_n, fp_frees)

static void __fastcall MultiMaster_Draw_n(MultiMaster*, Edx, gxCanvas*) {}
PORT_FN(0x0048c470, "MultiMaster::Draw", MultiMaster_Draw_n, fp_pure_draw)

static void* __fastcall MultiMaster_vdtor_n(MultiMaster* self, Edx, uint32_t flags) {
    tcall<void>(F_MultiMaster_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c480, "MultiMaster::vector deleting destructor", MultiMaster_vdtor_n, fp_frees)

static uint8_t __cdecl MultiMaster_Race_n(int32_t) { return tcall<uint8_t>(F_MultiMaster_race, g_master()); }
PORT_FN(0x0048c4a0, "MultiMaster::Race", MultiMaster_Race_n, fp_cb_client)
static uint8_t __cdecl MultiMaster_BackToX_n(int32_t) { return tcall<uint8_t>(F_MultiMaster_back_to_x, g_master()); }
PORT_FN(0x0048c4b0, "MultiMaster::BackToX", MultiMaster_BackToX_n, fp_cb_client)
static uint8_t __cdecl MultiMaster_Approve_n(int32_t) { return tcall<uint8_t>(F_MultiMaster_approve, g_master()); }
PORT_FN(0x0048c4c0, "MultiMaster::Approve", MultiMaster_Approve_n, fp_cb_client)

static void __fastcall MultiCarChooser_Draw_n(MultiCarChooser*, Edx, gxCanvas*) {}
PORT_FN(0x0048c4d0, "MultiCarChooser::Draw", MultiCarChooser_Draw_n, fp_pure_draw)

static void* __fastcall MultiCarChooser_sdtor_n(MultiCarChooser* self, Edx, uint32_t flags) {
    tcall<void>(F_MultiCarChooser_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c4e0, "MultiCarChooser::scalar deleting destructor", MultiCarChooser_sdtor_n, fp_frees)

static void* __fastcall MultiTrackChooser_sdtor_n(MultiTrackChooser* self, Edx, uint32_t flags) {
    tcall<void>(F_MultiTrackChooser_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c500, "MultiTrackChooser::scalar deleting destructor", MultiTrackChooser_sdtor_n, fp_frees)

static uint8_t __cdecl MTC_Prev_n(int32_t) {
    tcall<void>(F_MTC_prev, g_track());
    return 0;
}
static void fp_MTC_cb(Footprint& f, int32_t) { f.replay_only = "loads the track's stamp, reads the clock"; }
PORT_FN(0x0048c520, "MultiTrackChooser::Prev", MTC_Prev_n, fp_MTC_cb)
static uint8_t __cdecl MTC_Next_n(int32_t) {
    tcall<void>(F_MTC_next, g_track());
    return 0;
}
PORT_FN(0x0048c530, "MultiTrackChooser::Next", MTC_Next_n, fp_MTC_cb)

static void __fastcall MultiRaceCfg_Draw_n(MultiRaceCfg*, Edx, gxCanvas*) {}
PORT_FN(0x0048c540, "MultiRaceCfg::Draw", MultiRaceCfg_Draw_n, fp_pure_draw)

static void* __fastcall MultiRaceCfg_sdtor_n(MultiRaceCfg* self, Edx, uint32_t flags) {
    tcall<void>(F_MultiRaceCfg_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c550, "MultiRaceCfg::scalar deleting destructor", MultiRaceCfg_sdtor_n, fp_frees)

// a slider item (type 0xf), returned through the hidden pointer, in the original's store order
static UIDialogItem* __cdecl UI_SLIDER_n(UIDialogItem* out, int32_t x, int32_t y, int32_t w, int32_t h, float* v, uint32_t lo,
                                         uint32_t hi, int32_t steps) {
    volatile uint32_t* d = (volatile uint32_t*)out;
    d[0] = 0xf;
    d[1] = 0;
    d[2] = (uint32_t)x;
    d[6] = S_EMPTY_STR;
    d[3] = (uint32_t)y;
    d[4] = (uint32_t)w;
    d[5] = (uint32_t)h;
    d[9] = 0;
    d[7] = (uint32_t)steps;
    d[8] = U(v);
    d[12] = 0;
    d[13] = 0;
    d[10] = lo;
    d[11] = hi;
    return out;
}
// (not `pure`: it stores and returns pointers, which the offline fuzzer's two arenas can't compare)
static void fp_UI_SLIDER(Footprint& f, UIDialogItem* out, int32_t, int32_t, int32_t, int32_t, float*, uint32_t, uint32_t, int32_t) {
    f.add(out, sizeof(UIDialogItem), "the item");
}
PORT_FN(0x0048c570, "UI_SLIDER", UI_SLIDER_n, fp_UI_SLIDER)

static void __fastcall MultiRaceInfo_Draw_n(MultiRaceInfo*, Edx, gxCanvas*) {}
PORT_FN(0x0048c5d0, "MultiRaceInfo::Draw", MultiRaceInfo_Draw_n, fp_pure_draw)

static void* __fastcall MultiRaceInfo_sdtor_n(MultiRaceInfo* self, Edx, uint32_t flags) {
    tcall<void>(F_MultiRaceInfo_dtor, self);
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c5e0, "MultiRaceInfo::scalar deleting destructor", MultiRaceInfo_sdtor_n, fp_frees)

static void __fastcall LiveMultiKiller_Draw_n(LiveMultiKiller*, Edx, gxCanvas*) {}
PORT_FN(0x0048c600, "LiveMultiKiller::Draw", LiveMultiKiller_Draw_n, fp_pure_draw)

// the lobby's dialog opened: a live game ends (its chat resumed first); otherwise the lobby's lock is taken
static void __fastcall LiveMultiKiller_Create_n(LiveMultiKiller* self, Edx) {
    if (ccall<uint8_t>(F_MultiEnabled)) {
        ccall<void>(F_MultiResumeChat);
        ccall<void>(F_MultiLiveEnd);
        return;
    }
    net_Grab(self->lmi, 0);                                                  // site 0x48c62b
}
static void fp_LiveMultiKiller_Create(Footprint& f, LiveMultiKiller*, Edx) { f.replay_only = "ends the live game or takes the lobby's lock (the multi library)"; }
PORT_FN(0x0048c610, "LiveMultiKiller::Create", LiveMultiKiller_Create_n, fp_LiveMultiKiller_Create)

// the deleting destructor: UICustomControl's vtable put back, the object freed (flags & 1)
static void* __fastcall LiveMultiKiller_vdtor_n(LiveMultiKiller* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x0048c640, "LiveMultiKiller::vector deleting destructor", LiveMultiKiller_vdtor_n, fp_frees)

// the lobby's idle function: the dialog ends when the master's done
static uint8_t __cdecl MultiMaster_Idle_n(int32_t*) { return g_master()->done; }
static void fp_MultiMaster_Idle(Footprint&, int32_t*) {}
PORT_FN(0x0048c660, "MultiMaster::Idle", MultiMaster_Idle_n, fp_MultiMaster_Idle)

static uint8_t __cdecl MultiMaster_ExitButton_n(int32_t) { return tcall<uint8_t>(F_MultiMaster_exit_button, g_master()); }
static void fp_MultiMaster_ExitButton(Footprint& f, int32_t) { f.replay_only = "a yes / no box (modal)"; }
PORT_FN(0x0048c670, "MultiMaster::ExitButton", MultiMaster_ExitButton_n, fp_MultiMaster_ExitButton)

#undef RAW
#undef IC
#undef S
#undef G
#undef D
}  // namespace menu_sched
}  // namespace
