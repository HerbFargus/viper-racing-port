// menu_options.cpp -- M3 UI stage, step U2 (group B): moptions.obj and mmixer.obj, rewritten faithfully (library `menu`).
//
//   MenuDoOptions (the Options screen: five tabs -- Sound, Controls, Driving Aids, Graphics, Hacks -- built on its own
//   stack as UICustomControls, run by UIDoDialog, then saved), and each tab's control: its constructor (the options
//   read), Added (its items, _UIAddItems through UICustomControl::AddItems), Finalize (the options written back),
//   Create / Destroy / Callback / Update / Draw where it has them; the Controls tab's live test (ControlTestControl) and
//   its three defaults (default_ctl); the Graphics tab's benchmark (run_benchmark); the controls' adjust dialog
//   (adjust_ctls, its idle function adjust_idle, get_control_tweaks, tune_default) with its three bars (Meter,
//   SteerMeter); the Sound tab's driver chooser (MixerChooser, mmixer.obj).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (the toolkit's, the
// options', the driver's, this file's own), virtual calls through the vtable. Item lists are built in a frame laid out
// as the original's (the same offsets from its top), with UIDialogItem's constructor called where the original calls it
// and every dword stored where it stores the item inline; the translated texts read after their Xlator is refreshed,
// in the original's order. MenuDoOptions keeps its whole frame so: the five tab objects, the groups and the list sit where
// the original's do relative to each other.
//
// Footprints (main thread). replay_only: everything that reads or writes the options (OptionsGet creates a missing
// option; the constructors, the Finalizes, get_control_tweaks, FinalizeParent, run_benchmark), builds widgets or runs a
// dialog (MenuDoOptions, the Addeds, adjust_ctls, tune_default), adds or removes notifications (Create / Destroy), frees
// (the destructors), polls the input devices (ControlTestControl::Update, adjust_idle), restarts the sound or plays one
// (SoundOptionsControl::Callback's volume and quality, MixerChooser::Callback when the driver changes). The draws write
// the canvas they're given (and make it current); the Callbacks that only mark their widget dirty write the widget;
// default_ctl writes the Controls tab's first four bindings; MixerChooser's constructor itself and its variable;
// HackOptionsControl::Create and MixerChooser::Callback (driver unchanged) the running window's groups;
// SoundOptionsControl::Callback's strings (once its Xlators exist) the object and the two Xlators.
//
// Fixes (// FIX:): the Hacks tab's Vehicle list gets a scroll bar (HackOptionsControl::Added). It is built where the
// installed race.exe puts the list (stock, or moved by vrmod's car-list patch), read from the original's instructions.
// FIX CANDIDATEs (left faithful): see each `// FIX CANDIDATE:`.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "menu_options.h"

namespace {
namespace menu_options {
using namespace uit;
using namespace mob;

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
#define IC(n, ...) item_ctor(fr.at(n), __VA_ARGS__)
// an item the original stores inline: all 14 dwords
static __forceinline void raw(void* p, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t text,
                              uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi, uint32_t sel, uint32_t s34) {
    volatile uint32_t* d = (volatile uint32_t*)p;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = lo; d[11] = hi; d[12] = sel; d[13] = s34;
}
#define RAW(n, ...) raw(fr.at(n), __VA_ARGS__)
static __forceinline const char* sec(uint32_t s) { return UI_GP(const char, s); }

template <typename T> static void fp_replay_options0(Footprint& f, T*, Edx) { f.replay_only = "reads or writes the options (OptionsGet creates a missing one)"; }
template <typename T> static void fp_replay_items0(Footprint& f, T*, Edx) { f.replay_only = "builds widgets (_UIAddItems allocates)"; }
template <typename T> static void fp_replay_notes0(Footprint& f, T*, Edx) { f.replay_only = "adds or removes notifications (allocates or frees their copies)"; }
template <typename T> static void fp_draw_cc(Footprint& f, T*, Edx, gxCanvas* c) { mo_fp_draw(f, c); }
template <typename T> static void fp_dirty_cb(Footprint& f, T* self, Edx, int32_t, const void*) { mo_fp_dirty(f, self); }
template <typename T> static void fp_frees(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "frees the object"; }

// =====================================================================================================================
// mmixer.obj: MixerChooser
// =====================================================================================================================
static MixerChooser* __fastcall MixerChooser_ctor_n(MixerChooser* self, Edx, int32_t* sel, uint32_t* gc, uint32_t* gr) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_MixerChooser;
    self->sel = sel;
    self->grp_caps = gc;
    self->widget = 0;
    self->grp_real = gr;
    const int32_t d = ccall<int32_t>(F_MixerGetDefault);
    *(volatile int32_t*)sel = d;
    self->cur = d;
    return self;
}
static void fp_MixerChooser_ctor(Footprint& f, MixerChooser* self, Edx, int32_t* sel, uint32_t*, uint32_t*) {
    f.add(self, sizeof(MixerChooser), "this");
    f.add(sel, 4, "the mixer chosen");
}
PORT_FN(0x0049a950, "MixerChooser::MixerChooser", MixerChooser_ctor_n, fp_MixerChooser_ctor)

// one radio button per mixer (its name), 0x12 apart
static void __fastcall MixerChooser_Added_n(MixerChooser* self, Edx) {
    UIDialogItem it[2];
    const int32_t n = ccall<int32_t>(F_MixerCount);
    if (n <= 0) return;
    int32_t y = 0xfa;
    int32_t i = 0;
    do {
        void* m = ccall<void*>(F_MixerGet, i);
        const char* name = vcall<const char*>(m, 0);                // IMixer::GetName
        const uint32_t data = U(self->sel);
        raw(&it[0], 0xc, 0, 0x46, (uint32_t)y, 0, 0, U(name), (uint32_t)i, data, 8, 0, 0, 0, 0);
        y += 0x12;
        i++;
        item_end(&it[1]);
        add_items(self, &it[0]);
    } while (i < n);
}
PORT_FN(0x0049a990, "MixerChooser::Added", MixerChooser_Added_n, fp_replay_items0)

// the driver chosen: made the default and the sound restarted when it changed; then two groups shown or hidden by
// UIHideGroup / UIShowGroup (the table at 0x4fa1b8): the first by the mixer's caps' first byte, the second unless it's
// the null mixer
static void __fastcall MixerChooser_Callback_n(MixerChooser* self, Edx, int32_t, const void*) {
    int32_t* sel = self->sel;
    if (self->cur != *(volatile int32_t*)sel) {
        self->cur = *(volatile int32_t*)sel;
        ccall<void>(F_MixerSetDefault, *(volatile int32_t*)sel);
        ccall<void>(F_SoundRestart);
    }
    void* m = ccall<void*>(F_MixerGet, *(volatile int32_t*)self->sel);
    const uint32_t g1 = *(volatile uint32_t*)self->grp_caps;
    const uint8_t* caps = vcall<const uint8_t*>(m, 4);                 // IMixer::GetCaps
    // FIX CANDIDATE: a caps byte other than 0 or 1 indexes past the two-entry table (and calls what it finds there);
    // every mixer the game has sets 0 or 1
    ((void(__cdecl*)(uint32_t))(uintptr_t)UI_GU32(0x004fa1b8 + 4u * *(const volatile uint8_t*)caps))(g1);
    void* m2 = ccall<void*>(F_MixerGet, *(volatile int32_t*)self->sel);
    const uint32_t g2 = *(volatile uint32_t*)self->grp_real;
    const uint8_t null_mixer = vcall<uint8_t>(m2, 8);                 // IMixer::IsNullMixer
    ((void(__cdecl*)(uint32_t))(uintptr_t)UI_GU32(0x004fa1b8 + 4u * (null_mixer < 1 ? 1u : 0u)))(g2);
}
static void fp_MixerChooser_Callback(Footprint& f, MixerChooser* self, Edx, int32_t, const void*) {
    if (!self->sel || self->cur != *self->sel) { f.replay_only = "the driver changed: the sound restarts"; return; }
    f.add(self, sizeof(MixerChooser), "this");
    ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
    // SoftMixer::GetCaps fills a static MixerCaps and returns it (NullMixer's returns its own member; DSoundMixer's --
    // ds.obj, dead in every build -- is never among the mixers)
    f.add((void*)(uintptr_t)0x00578b30, 0xc, "SoftMixer's caps");
}
PORT_FN(0x0049aa60, "MixerChooser::Callback", MixerChooser_Callback_n, fp_MixerChooser_Callback)

static void __fastcall MixerChooser_Create_n(MixerChooser* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->sel, (uint32_t)4);
    vcall<void>(self, 0x10, (int32_t)0, (const void*)0);              // Callback(0, 0)
}
PORT_FN(0x0049aae0, "MixerChooser::Create", MixerChooser_Create_n, fp_replay_notes0)

static void __fastcall MixerChooser_Destroy_n(MixerChooser* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)self->sel);
}
PORT_FN(0x0049ab00, "MixerChooser::Destroy", MixerChooser_Destroy_n, fp_replay_notes0)

// =====================================================================================================================
// the bars: Meter, SteerMeter (adjust_ctls's)
// =====================================================================================================================
static void __fastcall Meter_Create_n(Meter* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)self->value, (uint32_t)4);
}
PORT_FN(0x00486670, "Meter::Create", Meter_Create_n, fp_replay_notes0)

static void __fastcall Meter_Callback_n(Meter* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty, self); }
PORT_FN(0x00486680, "Meter::Callback", Meter_Callback_n, fp_dirty_cb)

// the frame, then a bar from the left edge: *value of the width, the colour at 0x5798d4 (or, for a value whose bits are
// above 0x80000000 -- negative, or a negative NaN -- -*value of it in the colour at 0x5797c8)
static void __fastcall Meter_Draw_n(Meter* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = G(0x005799a4);
        ccall<void>(F_gxRect, x, y, self->w + x, self->h + y, col);
    }
    const float* v = self->value;
    if (!(*(const volatile uint32_t*)v > 0x80000000u)) {
        const int32_t y = self->y, x = self->x;
        const int32_t w = self->w;
        const uint32_t col = G(0x005798d4);
        const int32_t y1 = self->h + y - 1;
        const int32_t a = x87_ftol(D(w) * D(*(const volatile float*)v));
        ccall<void>(F_gxRect, x, y + 1, a + x, y1, col);
    } else {
        const int32_t y = self->y, x = self->x;
        const int32_t w = self->w;
        const uint32_t col = G(0x005797c8);
        const int32_t y1 = self->h + y - 1;
        const int32_t a = x87_ftol(-(D(w) * D(*(const volatile float*)v)));
        ccall<void>(F_gxRect, x, y + 1, a + x, y1, col);
    }
}
PORT_FN(0x00486690, "Meter::Draw", Meter_Draw_n, fp_draw_cc)

// the frame, then a bar from the middle: to (*value + 1) * (w / 2) from the left edge
static void __fastcall SteerMeter_Draw_n(Meter* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = G(0x005799a4);
        ccall<void>(F_gxRect, x, y, self->w + x, self->h + y, col);
    }
    const float* v = self->value;
    if (!(*(const volatile uint32_t*)v > 0x80000000u)) {
        const int32_t y = self->y;
        const int32_t half = self->w / 2;
        const int32_t x = self->x;
        const uint32_t col = G(0x005798d4);
        const double s = D(*(const volatile float*)v) + D(*(const volatile float*)(uintptr_t)0x004de488);
        const int32_t y1 = self->h + y - 1;
        const int32_t a = x87_ftol(s * D(half));
        ccall<void>(F_gxRect, x + half, y + 1, a + x, y1, col);
    } else {
        const int32_t y = self->y;
        const int32_t half = self->w / 2;
        const int32_t x = self->x;
        const uint32_t col = G(0x005797c8);
        const int32_t y1 = self->h + y - 1;
        const double s = D(*(const volatile float*)v) + D(*(const volatile float*)(uintptr_t)0x004de488);
        const int32_t a = x87_ftol(s * D(half));
        ccall<void>(F_gxRect, a + x, y + 1, x + half, y1, col);
    }
}
PORT_FN(0x00486760, "SteerMeter::Draw", SteerMeter_Draw_n, fp_draw_cc)

// =====================================================================================================================
// ControlTestControl: the Controls tab's live test
// =====================================================================================================================
// the bindings saved (the parent's): force feedback, game pad, then the ten controls
static void __fastcall CTC_FinalizeParent_n(ControlTestControl* self, Edx) {
    ControlsOptionsControl* p = self->parent;
    const uint8_t ff = p->force_feedback;
    const char* s = sec(S_SEC_CONTROL);
    opt_set_b(s, 0x004f7d58, ff);
    opt_set_b(s, 0x004f7d4c, p->game_pad);
    static const uint32_t keys[10] = {0x004f7d40, 0x004f7d34, 0x004f7d28, 0x004f7d20, 0x004f7d18,
                                      0x004f7d10, 0x004f7d08, 0x004f7d00, 0x004f7cf4, 0x004f7cec};
    for (int i = 0; i < 10; i++) control_set(&p->ctl[i], keys[i]);
}
PORT_FN(0x004818c0, "ControlTestControl::FinalizeParent", CTC_FinalizeParent_n, fp_replay_options0)

static void __fastcall CTC_dtor_n(ControlTestControl* self, Edx) {
    void* ball = self->ball;
    self->vtbl = (const void*)(uintptr_t)VT_ControlTest;
    ccall<void>(F_gxForgetStamp, ball);
    ccall<void>(F_gxForgetStamp, (void*)self->back);
    ccall<void>(F_DriverEnd);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
}
static void fp_CTC_dtor(Footprint& f, ControlTestControl*, Edx) { f.replay_only = "forgets its stamps, ends the driver"; }
PORT_FN(0x00484a80, "ControlTestControl::~ControlTestControl", CTC_dtor_n, fp_CTC_dtor)

static void __fastcall CTC_Create_n(ControlTestControl* self, Edx) {
    *(volatile uint32_t*)&self->timer = 0;
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->steer, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->brake, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->throttle, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->clutch, (uint32_t)4);
}
PORT_FN(0x00484ab0, "ControlTestControl::Create", CTC_Create_n, fp_replay_notes0)

// the steering wheel's ball: fld t; fcos; fmul -35; fsub 1; __ftol  /  fld t; fsin; fmul 35; fadd 0.5; __ftol -- the
// x87's fsin / fcos with their full 64-bit results, then __ftol's chop (docs/PORTING.md rule 9)
static int32_t ctc_cos_term(const volatile float* t) {
    int64_t q;
    uint16_t cw, chop;
    const volatile float* tp = t;
    __asm {
        mov eax, tp
        fld dword ptr [eax]
        fcos
        mov eax, 0x004de494
        fmul dword ptr [eax]
        mov eax, 0x004de488
        fsub dword ptr [eax]
        fnstcw cw
        mov ax, cw
        or ah, 0x0c
        mov chop, ax
        fldcw chop
        fistp qword ptr q
        fldcw cw
    }
    return (int32_t)q;
}
static int32_t ctc_sin_term(const volatile float* t) {
    int64_t q;
    uint16_t cw, chop;
    const volatile float* tp = t;
    __asm {
        mov eax, tp
        fld dword ptr [eax]
        fsin
        mov eax, 0x004de498
        fmul dword ptr [eax]
        mov eax, 0x004de49c
        fadd dword ptr [eax]
        fnstcw cw
        mov ax, cw
        or ah, 0x0c
        mov chop, ax
        fldcw chop
        fistp qword ptr q
        fldcw cw
    }
    return (int32_t)q;
}
// the panel: the wheel (its stamp, the ball at an angle of steer * 1.309 rad), three bars (throttle, brake, clutch)
// growing up from the bottom
static void __fastcall CTC_Draw_n(ControlTestControl* self, Edx, gxCanvas* c) {
    ccall<gxCanvas*>(F_gxSetCanvas, c);
    {
        const int32_t y = self->y, x = self->x;
        const uint32_t col = G(0x00579874);
        ccall<void>(F_gxRect, x, y, self->w + x, self->h + y, col);
    }
    const int32_t y0 = self->y;
    volatile float t;
    t = (float)(D(self->steer) * D(*(const volatile float*)(uintptr_t)0x004de490));
    const int32_t top = y0 + 0xd;
    const int32_t bottom = self->h + y0 - 0xd;
    const int32_t hgt = bottom - top;
    ccall<void>(F_gxDrawStamp, (const void*)self->back, self->x, y0, (int32_t)0, (const void*)0);
    const int32_t cy = ctc_cos_term(&t);
    const int32_t by = self->y + cy + 0x42;
    const int32_t cx = ctc_sin_term(&t);
    const int32_t bx = self->x + cx + 0x5f;
    ccall<void>(F_gxDrawStamp, (const void*)self->ball, bx, by, (int32_t)0, (const void*)0);
    const int32_t a1 = x87_ftol(D(self->throttle) * D(hgt));
    const int32_t t1 = bottom - a1;                                   // ebx
    const int32_t a2 = x87_ftol(D(self->brake) * D(hgt));
    const int32_t t2 = bottom - a2;                                   // [E-4]
    const int32_t a3 = x87_ftol(D(hgt) * D(self->clutch));
    const int32_t x0 = self->x + 0x92;                                // esi
    const int32_t t3 = bottom - a3;                                   // [E-20]
    const int32_t xa = x0 - 0x85, xb = x0 - 0x70;
    const uint32_t col_bar = G(0x005798d4), col_back = G(0x005799a4);
    ccall<void>(F_gxRect, xa, t3, xa + 0xb, bottom, col_bar);
    ccall<void>(F_gxRect, xa, top, xa + 0xb, t3, col_back);
    ccall<void>(F_gxRect, xb, t2, xb + 0xb, bottom, col_bar);
    ccall<void>(F_gxRect, xb, top, xb + 0xb, t2, col_back);
    ccall<void>(F_gxRect, x0, t1, x0 + 0xb, bottom, col_bar);
    ccall<void>(F_gxRect, x0, top, x0 + 0xb, t1, col_back);
}
PORT_FN(0x00484b00, "ControlTestControl::Draw", CTC_Draw_n, fp_draw_cc)

static void __fastcall CTC_Callback_n(ControlTestControl* self, Edx, int32_t, const void*) { tcall<void>(F_UICC_Dirty, self); }
PORT_FN(0x00484cf0, "ControlTestControl::Callback", CTC_Callback_n, fp_dirty_cb)

// every 0.25 s the bindings are saved and the driver refreshed; each frame the driver is updated and read
static void __fastcall CTC_Update_n(ControlTestControl* self, Edx) {
    const double s = D(ccall<float>(F_UIDeltaT)) + D(self->timer);
    self->timer = (float)s;
    if (s > D(*(const volatile float*)(uintptr_t)0x004de4a0)) {
        self->timer = (float)(D(self->timer) - D(*(const volatile float*)(uintptr_t)0x004de4a0));
        tcall<void>(F_CTC_FinalizeParent, self);
        ccall<void>(F_DriverRefresh);
    }
    ccall<void>(F_DriverUpdate, ccall<float>(F_UIDeltaT));
    self->steer = ccall<float>(F_DriverGetSteering, 0.0f);
    self->brake = ccall<float>(F_DriverGetBraking);
    self->throttle = ccall<float>(F_DriverGetThrottle);
    self->clutch = ccall<float>(F_DriverGetClutch);
    tcall<void>(F_UICC_Dirty, self);
}
static void fp_CTC_Update(Footprint& f, ControlTestControl*, Edx) { f.replay_only = "polls the input devices (DriverUpdate), saves the bindings"; }
PORT_FN(0x00484d00, "ControlTestControl::Update", CTC_Update_n, fp_CTC_Update)

// the vector deleting destructor (the destructor inlined; no array form)
static void* __fastcall CTC_vdtor_n(ControlTestControl* self, Edx, uint32_t flags) {
    void* ball = self->ball;
    self->vtbl = (const void*)(uintptr_t)VT_ControlTest;
    ccall<void>(F_gxForgetStamp, ball);
    ccall<void>(F_gxForgetStamp, (void*)self->back);
    ccall<void>(F_DriverEnd);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00484d80, "ControlTestControl::vector deleting destructor", CTC_vdtor_n, fp_frees)

// =====================================================================================================================
// ControlsOptionsControl: the Controls tab
// =====================================================================================================================
static ControlsOptionsControl* __fastcall Controls_ctor_n(ControlsOptionsControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    self->test.widget = 0;
    self->test.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->test.vtbl = (const void*)(uintptr_t)VT_ControlTest;
    ccall<uint8_t>(F_DriverBegin);
    self->test.ball = ccall<void*>(F_gxGetStamp, (const char*)0x004f7f50);
    self->test.back = ccall<void*>(F_gxGetStamp, (const char*)0x004f7f44);
    *(volatile uint32_t*)&self->test.clutch = 0;
    *(volatile uint32_t*)&self->test.brake = 0;
    *(volatile uint32_t*)&self->test.throttle = 0;
    *(volatile uint32_t*)&self->test.steer = 0;
    self->test.parent = 0;
    self->vtbl = (const void*)(uintptr_t)VT_ControlsOptions;
    UI_GP(ControlsOptionsControl, S_CONTROLS_GLOBAL) = self;
    self->test.parent = self;
    self->force_feedback = 0;
    self->game_pad = 0;
    const char* s = sec(S_SEC_CONTROL);
    opt_get_b(s, 0x004f7d58, &self->force_feedback);
    opt_get_b(s, 0x004f7d4c, &self->game_pad);
    static const uint32_t keys[10] = {0x004f7d40, 0x004f7d34, 0x004f7d28, 0x004f7d20, 0x004f7d18,
                                      0x004f7d10, 0x004f7d08, 0x004f7d00, 0x004f7cf4, 0x004f7cec};
    for (int i = 0; i < 10; i++) control_get(&self->ctl[i], keys[i]);
    return self;
}
PORT_FN(0x00484dd0, "ControlsOptionsControl::ControlsOptionsControl", Controls_ctor_n, fp_replay_options0)

// the test panel, Tune (adjust_ctls), the three defaults' pictures (default_ctl), the bindings (a CDetect each), the
// force feedback and game pad boxes
static void __fastcall Controls_Added_n(ControlsOptionsControl* self, Edx) {
    Frame<0x690> fr;
    IC(0x690, 0x17, 0, 0x1a4, 0xbe, 0xaa, 0x85, S_EMPTY, 0, S(0x6c), 0, 0, 0, 0, 0);
    xl(0x00579af0);
    IC(0x658, 2, 0x3e7, 0x3c, 0x127, 0, 0, G(0x00579af4), 0, F_adjust_ctls, 0, 0, 0, 0, 0);
    IC(0x620, 3, 0, 0x1ad, 0x148, 0, 0, 0x004f7f74, 0, F_default_ctl, 0, 0, 0, 0, 0);
    IC(0x5e8, 3, 1, 0x1df, 0x148, 0, 0, 0x004f7f68, 0, F_default_ctl, 0, 0, 0, 0, 0);
    IC(0x5b0, 3, 2, 0x211, 0x148, 0, 0, 0x004f7f5c, 0, F_default_ctl, 0, 0, 0, 0, 0);
    xl(0x00579cd8);
    IC(0x578, 5, 0, 0x28, 0x82, 0, 0, G(0x00579cdc), 0, 0, 0xb, 0, 0, 0, 0);
    uint32_t name;
    if (*(const volatile char*)ccall<const char*>(F_JoyGetName)) {
        name = U(ccall<const char*>(F_JoyGetName));
    } else {
        xl(0x005798b8);
        name = G(0x005798bc);
    }
    IC(0x540, 5, 0, 0x32, 0x15e, 0, 0, name, 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    xl(0x00579ca8);
    IC(0x508, 5, 0, 0x32, 0x96, 0, 0, G(0x00579cac), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579988);
    IC(0x4d0, 0xa, 0, 0x8c, 0x96, 0x50, 0x10, G(0x0057998c), 0, S(0x1c), 9, 0, 0, 0, 0);
    xl(0x005797d0);
    IC(0x498, 5, 0, 0x32, 0xaa, 0, 0, G(0x005797d4), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579d38);
    IC(0x460, 0xa, 0, 0x8c, 0xaa, 0x50, 0x10, G(0x00579d3c), 0, S(0x24), 9, 0, 0, 0, 0);
    xl(0x005797b8);
    IC(0x428, 5, 0, 0x32, 0xbe, 0, 0, G(0x005797bc), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579be8);
    IC(0x3f0, 0xa, 0, 0x8c, 0xbe, 0x50, 0x10, G(0x00579bec), 0, S(0x2c), 9, 0, 0, 0, 0);
    xl(0x00579a30);
    IC(0x3b8, 5, 0, 0x32, 0xd2, 0, 0, G(0x00579a34), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579c48);
    IC(0x380, 0xa, 0, 0x8c, 0xd2, 0x50, 0x10, G(0x00579c4c), 0, S(0x34), 9, 0, 0, 0, 0);
    xl(0x005799d8);
    IC(0x348, 5, 0, 0x32, 0xe6, 0, 0, G(0x005799dc), 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    xl(0x00579820);
    IC(0x310, 0xa, 0, 0x8c, 0xe6, 0x50, 0x10, G(0x00579824), 0, S(0x44), 9, 0, 0, 0, 0);
    xl(0x00579cf8);
    IC(0x2d8, 5, 0, 0xf0, 0x96, 0, 0, G(0x00579cfc), 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    xl(0x00579a50);
    IC(0x2a0, 0xa, 0, 0x14a, 0x96, 0x50, 0x10, G(0x00579a54), 0, S(0x54), 9, 0, 0, 0, 0);
    xl(0x00579b40);
    IC(0x268, 5, 0, 0xf0, 0xaa, 0, 0, G(0x00579b44), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579a60);
    IC(0x230, 0xa, 0, 0x14a, 0xaa, 0x50, 0x10, G(0x00579a64), 0, S(0x5c), 9, 0, 0, 0, 0);
    xl(0x00579a70);
    IC(0x1f8, 5, 0, 0xf0, 0xbe, 0, 0, G(0x00579a74), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579908);
    IC(0x1c0, 0xa, 0, 0x14a, 0xbe, 0x50, 0x10, G(0x0057990c), 0, S(0x4c), 9, 0, 0, 0, 0);
    xl(0x005799a8);
    IC(0x188, 5, 0, 0xf0, 0xd2, 0, 0, G(0x005799ac), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579c28);
    IC(0x150, 0xa, 0, 0x14a, 0xd2, 0x50, 0x10, G(0x00579c2c), 0, S(0x3c), 9, 0, 0, 0, 0);
    xl(0x00579c38);
    RAW(0x118, 5, 0, 0xf0, 0xe6, 0, 0, G(0x00579c3c), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579d28);
    IC(0xe0, 0xa, 0, 0x14a, 0xe6, 0x50, 0x10, G(0x00579d2c), 0, S(0x64), 9, 0, 0, 0, 0);
    xl(0x00579998);
    IC(0xa8, 0xb, 0, 0x1b3, 0x96, 0, 0, G(0x0057999c), 0, S(0x18), 7, 0, 0, 0, 0);
    xl(0x005798e0);
    IC(0x70, 0xb, 0, 0x1b3, 0xaa, 0, 0, G(0x005798e4), 0, S(0x19), 7, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x690));
}
PORT_FN(0x00484f30, "ControlsOptionsControl::Added", Controls_Added_n, fp_replay_items0)

// the defaults: 0 keyboard, 1 joystick, 2 wheel -- the first four bindings (steer left / right, throttle, braking) of
// ControlsOptionsControl::global
static uint8_t __cdecl default_ctl_n(int32_t which) {
    uint8_t* g = (uint8_t*)UI_GP(ControlsOptionsControl, S_CONTROLS_GLOBAL);
    switch (which) {
    case 0:
        *(volatile uint32_t*)(g + 0x1c) = 1;
        *(volatile uint8_t*)(g + 0x20) = 0xcb;
        *(volatile uint32_t*)(g + 0x24) = 1;
        *(volatile uint8_t*)(g + 0x28) = 0xcd;
        *(volatile uint32_t*)(g + 0x2c) = 1;
        *(volatile uint8_t*)(g + 0x30) = 0xc8;
        *(volatile uint32_t*)(g + 0x34) = 1;
        *(volatile uint8_t*)(g + 0x38) = 0xd0;
        return 0;
    case 1:
        *(volatile uint32_t*)(g + 0x1c) = 2;
        *(volatile uint32_t*)(g + 0x20) = 0;
        *(volatile uint32_t*)(g + 0x24) = 2;
        *(volatile uint32_t*)(g + 0x28) = 1;
        *(volatile uint32_t*)(g + 0x2c) = 3;
        *(volatile uint32_t*)(g + 0x30) = 0;
        *(volatile uint32_t*)(g + 0x34) = 3;
        *(volatile uint32_t*)(g + 0x38) = 1;
        return 0;
    case 2:
        *(volatile uint32_t*)(g + 0x1c) = 2;
        *(volatile uint32_t*)(g + 0x20) = 0;
        *(volatile uint32_t*)(g + 0x24) = 2;
        *(volatile uint32_t*)(g + 0x28) = 1;
        *(volatile uint32_t*)(g + 0x2c) = 2;
        *(volatile uint32_t*)(g + 0x30) = 2;
        *(volatile uint32_t*)(g + 0x34) = 2;
        *(volatile uint32_t*)(g + 0x38) = 3;
        return 0;
    }
    UI_LogPanic((const char*)0x004f7f80, which);
    return 0;
}
static void fp_default_ctl(Footprint& f, int32_t) {
    uint8_t* g = (uint8_t*)UI_GP(ControlsOptionsControl, S_CONTROLS_GLOBAL);
    if (g) f.add(g + 0x1c, 0x20, "ControlsOptionsControl::global's first four bindings");
}
PORT_FN(0x004858b0, "ControlsOptionsControl::default_ctl", default_ctl_n, fp_default_ctl)

static void __fastcall Controls_Finalize_n(ControlsOptionsControl* self, Edx) {
    const uint8_t ff = self->force_feedback;
    const char* s = sec(S_SEC_CONTROL);
    opt_set_b(s, 0x004f7d58, ff);
    opt_set_b(s, 0x004f7d4c, self->game_pad);
    static const uint32_t keys[10] = {0x004f7d40, 0x004f7d34, 0x004f7d28, 0x004f7d20, 0x004f7d18,
                                      0x004f7d10, 0x004f7d08, 0x004f7d00, 0x004f7cf4, 0x004f7cec};
    for (int i = 0; i < 10; i++) control_set(&self->ctl[i], keys[i]);
}
PORT_FN(0x00485960, "ControlsOptionsControl::Finalize", Controls_Finalize_n, fp_replay_options0)

// the scalar deleting destructor: the test panel's destructor inlined, then the control's
static void* __fastcall Controls_sdtor_n(ControlsOptionsControl* self, Edx, uint32_t flags) {
    void* ball = self->test.ball;
    self->test.vtbl = (const void*)(uintptr_t)VT_ControlTest;
    ccall<void>(F_gxForgetStamp, ball);
    ccall<void>(F_gxForgetStamp, (void*)self->test.back);
    ccall<void>(F_DriverEnd);
    self->test.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00485a50, "ControlsOptionsControl::scalar deleting destructor", Controls_sdtor_n, fp_frees)

// =====================================================================================================================
// AidsOptionsControl: the Driving Aids tab
// =====================================================================================================================
static AidsOptionsControl* __fastcall Aids_ctor_n(AidsOptionsControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_AidsOptions;
    self->abs = 2;
    self->widget = 0;
    const char* s = sec(S_SEC_CONTROL);
    self->yaw = 0;
    self->auto_shifting = 1;
    self->traction = 1;
    opt_get_i(s, 0x004f7fc8, &self->traction);
    opt_get_i(s, 0x004f7fbc, &self->yaw);
    opt_get_i(s, 0x004f7fb0, &self->abs);
    opt_get_b(s, 0x004f7fa0, &self->auto_shifting);
    // FIX CANDIDATE: auto_clutch (+0x19) is never set before this OptionsGet, which creates a missing option from
    // whatever the byte held (MenuDoOptions's stack); the options file always has it after the first run
    opt_get_b(s, 0x004f7f94, &self->auto_clutch);
    if (self->abs > 2) self->abs = 2;
    if (self->traction > 2) self->traction = 2;
    return self;
}
PORT_FN(0x00485aa0, "AidsOptionsControl::AidsOptionsControl", Aids_ctor_n, fp_replay_options0)

static void __fastcall Aids_Added_n(AidsOptionsControl* self, Edx) {
    Frame<0x230> fr;
    static const uint32_t bufs[3] = {S_AIDS_OPTS1, S_AIDS_OPTS2, S_AIDS_OPTS3};
    for (int k = 0; k < 3; k++) {
        xl(0x00579bb8);
        xl(0x00579958);
        xl(0x00579948);
        ccall<void>(F_CreateMultiString, (char*)(uintptr_t)bufs[k], G(0x0057994c), G(0x0057995c), G(0x00579bbc), (uint32_t)0);
    }
    xl(0x00579b60);
    RAW(0x230, 5, 0, 0x28, 0x82, 0, 0, G(0x00579b64), 0, 0, 0xb, 0, 0, 0, 0);
    xl(0x00579a10);
    IC(0x1f8, 0xb, 0, 0x32, 0x96, 0, 0, G(0x00579a14), 0, S(0x18), 7, 0, 0, 0, 0);
    xl(0x00579ab0);
    IC(0x1c0, 0xb, 0, 0x32, 0xaa, 0, 0, G(0x00579ab4), 0, S(0x19), 7, 0, 0, 0, 0);
    xl(0x00579a00);
    IC(0x188, 5, 0, 0x32, 0xbe, 0, 0, G(0x00579a04), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x150, 8, 0, 0xaa, 0xbe, 0, 0, S_AIDS_OPTS2, 0, S(0x20), 0x15, 0, 0, 0, 0);
    xl(0x005799f0);
    RAW(0x118, 5, 0, 0x32, 0xd2, 0, 0, G(0x005799f4), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0xe0, 8, 0, 0xaa, 0xd2, 0, 0, S_AIDS_OPTS1, 0, S(0x1c), 0x15, 0, 0, 0, 0);
    xl(0x00579898);
    IC(0xa8, 5, 0, 0x32, 0xe6, 0, 0, G(0x0057989c), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x70, 8, 0, 0xaa, 0xe6, 0, 0, S_AIDS_OPTS3, 0, S(0x24), 0x15, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x230));
}
PORT_FN(0x00485b50, "AidsOptionsControl::Added", Aids_Added_n, fp_replay_items0)

static void __fastcall Aids_Finalize_n(AidsOptionsControl* self, Edx) {
    const int32_t tc = self->traction;
    const char* s = sec(S_SEC_CONTROL);
    opt_set_i(s, 0x004f7fc8, tc);
    opt_set_i(s, 0x004f7fb0, self->abs);
    opt_set_b(s, 0x004f7fa0, self->auto_shifting);
    opt_set_b(s, 0x004f7f94, self->auto_clutch);
    opt_set_i(s, 0x004f7fbc, self->yaw);
}
PORT_FN(0x00485fa0, "AidsOptionsControl::Finalize", Aids_Finalize_n, fp_replay_options0)

// =====================================================================================================================
// HackOptionsControl: the Hacks tab
// =====================================================================================================================
static HackOptionsControl* __fastcall Hack_ctor_n(HackOptionsControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    const int32_t n = ccall<int32_t>(F_GetMaxCarFileNames);
    tcall<void*>(F_UIStringList_ctor, (void*)&self->cars, n + 1, (int32_t)0x10);
    self->vtbl = (const void*)(uintptr_t)VT_HackOptions;
    self->show_status = 0;
    self->horn_ball = 0;
    self->no_walls = 0;
    self->pave = 0;
    *(volatile uint32_t*)&self->throttle_boost = 0x3f800000u;
    *(volatile uint32_t*)&self->grip_boost = 0x3f800000u;
    *(volatile uint32_t*)&self->gravity = 0x3f800000u;
    self->car_index = ccall<int32_t>(F_GetCarFileNumber, (const char*)0x004f805c);
    const char* s = sec(S_SEC_GAME);
    opt_get_b(s, 0x004f8044, &self->show_status);
    opt_get_f(s, 0x004f8034, &self->throttle_boost);
    opt_get_f(s, 0x004f8028, &self->grip_boost);
    opt_get_f(s, 0x004f8018, &self->gravity);
    opt_get_b(s, 0x004f800c, &self->horn_ball);
    opt_get_b(s, 0x004f8000, &self->no_walls);
    opt_get_b(s, 0x004f7ff0, &self->pave);
    opt_get_i(s, 0x004f7fe4, &self->car_index);
    int32_t i = 0;
    if (ccall<int32_t>(F_GetMaxCarFileNames) > 0) {
        do {
            const char* name = ccall<const char*>(F_GetCarFileName, i);
            i++;
            tcall<void>(F_UIStringList_AddEntry, (void*)&self->cars, name);
        } while (ccall<int32_t>(F_GetMaxCarFileNames) > i);
    }
    control_get(&self->help, 0x004f7fdc);
    return self;
}
PORT_FN(0x00486040, "HackOptionsControl::HackOptionsControl", Hack_ctor_n, fp_replay_options0)

static void __fastcall Hack_Finalize_n(HackOptionsControl* self, Edx) {
    const uint8_t st = self->show_status;
    const char* s = sec(S_SEC_GAME);
    opt_set_b(s, 0x004f8044, st);
    opt_set_f(s, 0x004f8034, *(volatile uint32_t*)&self->throttle_boost);
    opt_set_f(s, 0x004f8028, *(volatile uint32_t*)&self->grip_boost);
    opt_set_f(s, 0x004f8018, *(volatile uint32_t*)&self->gravity);
    opt_set_b(s, 0x004f800c, self->horn_ball);
    opt_set_b(s, 0x004f8000, self->no_walls);
    opt_set_b(s, 0x004f7ff0, self->pave);
    opt_set_i(s, 0x004f7fe4, self->car_index);
    control_set(&self->help, 0x004f7fdc);
    ccall<void>(F_HackRefresh);
}
PORT_FN(0x00486170, "HackOptionsControl::Finalize", Hack_Finalize_n, fp_replay_options0)

// The Vehicle list's place and size: the four immediates of the original's `push h / push w (imm8) / push y / push x`
// at 0x48635f, read from the installed race.exe's own instructions (as m1_operand reads M1's values: docs/PORTING.md rule
// 12). Stock v1.0 has (300, 200, 100, 200); vrmod's car-list patch (viper-mod-manager vrmod/carlist.py) rewrites the four
// values -- (390, 124, 111, 255) by default, the community race.bin builds' --, and port_check_stock accepts a
// HackOptionsControl::Added that differs from stock only there (stock.inc's VP_STOCK_MASKS), so this rewrite runs in
// game either way and builds the list where the installed game would. Anything else at those bytes: stock's.
struct ListGeom { uint32_t x, y, w, h; };
static ListGeom vehicle_list_geometry() {
    const volatile uint8_t* p = (const volatile uint8_t*)(uintptr_t)0x0048635f;
    auto d32 = [](const volatile uint8_t* q) { return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24; };
    if (p[0] == 0x68 && p[5] == 0x6a && p[7] == 0x68 && p[12] == 0x68 && p[17] == 0x51 && p[18] == 0x6a && p[19] == 0x14)
        return {d32(p + 13), d32(p + 8), (uint32_t)(int32_t)(int8_t)p[6], d32(p + 1)};   // push imm8: sign-extended
    return {0x12c, 0xc8, 0x64, 0xc8};
}

// FIX: the Vehicle list (the only place to change cars) had no scroll bar, so it showed its first h / 15 cars (13 stock,
// 17 with vrmod's taller list) and any car after them in the sorted list couldn't be chosen. It gets one, built as the
// game's own scrolling lists are (UIDoOpenFileBox's): a UIScrollAxis given to the 0x14 ListBox as its text, then a 0x11
// item (a vertical ScrollBar and its two ScrollButtons, uscroll.stp / dscroll.stp 15 x 18, the track vslider.stp 16
// wide) on the same axis. The bar takes the list's right 16 pixels and the list keeps the rest less a 2-pixel gap, so
// nothing else on the screen moves: the list's rectangle is the original's. The axis lives here (the tab's object has no
// room; it sits in MenuDoOptions's frame among the other tabs), one at a time as the Options screen is. When the chosen
// car is below the first page the list opens scrolled to show it at the top (kept to the last page). A list too small
// for a bar (only a custom vrmod size under 64 x 32) is built as before.
static UIScrollAxis s_vehicle_axis;
enum : uint32_t { BAR_W = 0x10, BAR_GAP = 2 };

static void __fastcall Hack_Added_n(HackOptionsControl* self, Edx) {
    Frame<VP_FIX ? 0x1f8 : 0x1c0> fr;                     // FIX: (above) room for one more item
    const ListGeom g = vehicle_list_geometry();
    const bool bar = VP_FIX && (int32_t)g.h >= 0x40 && (int32_t)g.w >= 0x20;
    const uint32_t k = bar ? 0x38u : 0u;                  // the items before the bar sit one item further up the frame
    once_xl(S_HACK_ADDED_ONCE, 1, 0x005d5bb0, 0x004f80ac, 0x00486630);
    once_xl(S_HACK_ADDED_ONCE, 2, 0x005d5bd0, 0x004f8094, 0x00486620);
    once_xl(S_HACK_ADDED_ONCE, 4, 0x005d5be0, 0x004f8078, 0x00486610);
    once_xl(S_HACK_ADDED_ONCE, 8, 0x005d5bc0, 0x004f8064, 0x00486600);
    xl(0x005d5bb0);
    RAW(0x1c0 + k, 5, 0, 0x122, 0x82, 0, 0, G(0x005d5bb4), 0, 0, 0xb, 0, 0, 0, 0);
    if (bar) {
        s_vehicle_axis.total = 0;
        s_vehicle_axis.visible = 0;
        s_vehicle_axis.pos = 0;
        IC(0x188 + k, 0x14, 0, g.x, g.y, g.w - BAR_W - BAR_GAP, g.h, U(&s_vehicle_axis), 0, S(0x28), 0, 0, 0, S(0x3c), 0);
        IC(0x188, 0x11, 0, g.x + g.w - BAR_W, g.y, BAR_W, g.h, S_EMPTY, 0, U(&s_vehicle_axis), 0, 0, 0, 0, 0);
    } else {
        IC(0x188, 0x14, 0, g.x, g.y, g.w, g.h, 0, 0, S(0x28), 0, 0, 0, S(0x3c), 0);
    }
    xl(0x005d5bc0);
    IC(0x150, 5, 0, 0x28, 0x82, 0, 0, G(0x005d5bc4), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    xl(0x005d5bd0);
    IC(0x118, 0xb, 0, 0x32, 0x9b, 0, 0, G(0x005d5bd4), 0, S(0x19), 7, 0, 0, 0, 0);
    xl(0x005d5be0);
    IC(0xe0, 0xb, 0, 0x32, 0xaf, 0, 0, G(0x005d5be4), 0, S(0x1b), 7, 0, 0, 0, 0);
    xl(0x00579aa0);
    RAW(0xa8, 5, 0, 0x32, 0xc8, 0, 0, G(0x00579aa4), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579b20);
    RAW(0x70, 0xa, 0, 0x8c, 0xc8, 0x50, 0x10, G(0x00579b24), 0, S(0x40), 9, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x1c0 + k));
    if (bar) {                                            // FIX: (above) the chosen car in view
        UIScrollAxis* a = &s_vehicle_axis;
        const int32_t sel = self->car_index;
        if (sel >= a->visible && a->visible > 0) {
            int32_t p = sel;
            const int32_t last = a->total - a->visible;
            if (p > last) p = last;
            a->pos = p < 0 ? 0 : p;
        }
    }
}
PORT_FN(0x00486230, "HackOptionsControl::Added", Hack_Added_n, fp_replay_items0)

// the tab hidden unless the hacks are enabled
static void __fastcall Hack_Create_n(HackOptionsControl*, Edx) {
    if (ccall<uint8_t>(F_HackEnabled)) return;
    ccall<void>(F_UIHideGroup, G(S_HACK_GROUP));
}
static void fp_Hack_Create(Footprint& f, HackOptionsControl*, Edx) { ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE)); }
PORT_FN(0x004865d0, "HackOptionsControl::Create", Hack_Create_n, fp_Hack_Create)

static void* __fastcall Hack_vdtor_n(HackOptionsControl* self, Edx, uint32_t flags) {
    tcall<void>(F_UIStringList_dtor, (void*)&self->cars);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00486640, "HackOptionsControl::vector deleting destructor", Hack_vdtor_n, fp_frees)

// =====================================================================================================================
// GfxOptionsControl: the Graphics tab
// =====================================================================================================================
static GfxOptionsControl* __fastcall Gfx_ctor_n(GfxOptionsControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->vtbl = (const void*)(uintptr_t)VT_GfxOptions;
    self->widget = 0;
    self->video_mode = 2;
    self->filtering = 1;
    *(volatile uint32_t*)&self->detail_level = 0x3f800000u;
    *(volatile uint32_t*)&self->draw_distance = 0x3f800000u;
    self->sky = 1;
    self->road = 1;
    self->wall = 1;
    self->smoke = 1;
    self->fog = 1;
    self->lighting = 1;
    self->specular = 0;
    self->car = 1;
    self->mirror = 0;
    const char* s = sec(S_SEC_GX);
    self->skids = 0;
    self->mipmap = 0;
    self->shadow = 0;
    opt_get_f(s, 0x004f7e3c, &self->draw_distance);
    opt_get_f(s, 0x004f7e2c, &self->detail_level);
    opt_get_b(s, 0x004f7e20, &self->sky);
    opt_get_b(s, 0x004f7e10, &self->road);
    opt_get_b(s, 0x004f7e04, &self->car);
    opt_get_b(s, 0x004f7df4, &self->wall);
    opt_get_i(s, 0x004f7dec, &self->smoke);
    opt_get_b(s, 0x004f7de0, &self->filtering);
    opt_get_i(s, 0x004f7dd4, &self->video_mode);
    opt_get_b(s, 0x004f7dd0, &self->fog);
    opt_get_b(s, 0x004f7dc4, &self->lighting);
    opt_get_i(s, 0x004f7db8, &self->specular);
    opt_get_i(s, 0x004f7db0, &self->mirror);
    opt_get_b(s, 0x004f7da8, &self->mipmap);
    opt_get_i(s, 0x004f7da0, &self->skids);
    opt_get_i(s, 0x004f7d98, &self->shadow);
    if (!ccall<uint8_t>(F_VidIsModeSupported, self->video_mode))
        self->video_mode = ccall<int32_t>(F_VidGetMegsRam) > 2 ? 2 : 1;
    return self;
}
PORT_FN(0x00483050, "GfxOptionsControl::GfxOptionsControl", Gfx_ctor_n, fp_replay_options0)

// no 3D hardware: a warning alone. Else the settings -- quality, draw distance, detail (sliders), the textures, fog,
// lighting, reflections, smoke, shadows, skids, mirror (multi strings OFF / LOW / MED / HI), the benchmark button
// with its frames per second -- and then the video modes the card supports, as radio buttons in two columns
static void __fastcall Gfx_Added_n(GfxOptionsControl* self, Edx) {
    Frame<0x698> fr;
    if (!ccall<uint8_t>(F_VidHas3DHW)) {
        xl(0x00579c70);
        IC(0x690, 5, 0, 0x82, 0xf0, 0, 0, G(0x00579c74), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
        item_end(fr.at(0x658));
        add_items(self, fr.at(0x690));
        return;
    }
    xl(0x00579bb8);
    xl(0x00579918);
    xl(0x00579958);
    xl(0x00579948);
    ccall<void>(F_CreateMultiString, (char*)(uintptr_t)S_MIRROR_OPTS, G(0x0057994c), G(0x0057995c), G(0x0057991c), G(0x00579bbc),
                (uint32_t)0);
    xl(0x00579bb8);
    xl(0x00579958);
    xl(0x00579948);
    ccall<void>(F_CreateMultiString, (char*)(uintptr_t)S_SPECULAR_OPTS, G(0x0057994c), G(0x0057995c), G(0x00579bbc), (uint32_t)0);
    xl(0x00579858);
    IC(0x620, 5, 0, 0x28, 0xc8, 0, 0, G(0x0057985c), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579888);
    IC(0x5e8, 5, 0, 0x28, 0x82, 0, 0, G(0x0057988c), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579838);
    IC(0x5b0, 2, 0x3e7, 0x190, 0x96, 0, 0, G(0x0057983c), 0, F_run_benchmark, 0, 0, 0, 0, 0);
    xl(0x00579b70);
    IC(0x578, 5, 0, 0x1f9, 0xa0, 0, 0, G(0x00579b74), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x540, 6, 0, 0x22b, 0x9e, 0, 0, 0x004f7e7c, 0, S_BENCH_FPS, 0x15, 0x3f800000, 0, 0, 0);
    xl(0x005798a8);
    IC(0x508, 5, 0, 0x32, 0xdc, 0, 0, G(0x005798ac), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    RAW(0x4d0, 0xf, 0, 0xfa, 0xdc, 0x64, 8, S_EMPTY, (uint32_t)-5, S(0x24), 0, 0, 0x3f800000, 0, 0);
    xl(0x00579b30);
    IC(0x498, 5, 0, 0x32, 0xf0, 0, 0, G(0x00579b34), 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    IC(0x460, 0xf, 0, 0xfa, 0xf0, 0x64, 8, S_EMPTY, (uint32_t)-0xb, S(0x20), 0, 0, 0x3f800000, 0, 0);
    xl(0x00579c98);
    RAW(0x428, 5, 0, 0x28, 0x10e, 0, 0, G(0x00579c9c), 0, 0, 0xb, 0, 0, 0, 0);
    xl(0x00579848);
    IC(0x3f0, 0xb, 0, 0x3c, 0x122, 0, 0, G(0x0057984c), 0, S(0x1c), 7, 0, 0, 0, 0);
    xl(0x00579978);
    IC(0x3b8, 0xb, 0, 0x3c, 0x136, 0, 0, G(0x0057997c), 0, S(0x38), 7, 0, 0, 0, 0);
    xl(0x005797a8);
    IC(0x380, 0xb, 0, 0xfa, 0x122, 0, 0, G(0x005797ac), 0, S(0x39), 7, 0, 0, 0, 0);
    xl(0x005799c8);
    IC(0x348, 0xb, 0, 0xfa, 0x136, 0, 0, G(0x005799cc), 0, S(0x40), 7, 0, 0, 0, 0);
    xl(0x005798f0);
    IC(0x310, 5, 0, 0x28, 0x14f, 0, 0, G(0x005798f4), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    xl(0x00579ae0);
    IC(0x2d8, 0xb, 0, 0x32, 0x163, 0, 0, G(0x00579ae4), 0, S(0x28), 7, 0, 0, 0, 0);
    xl(0x00579a80);
    RAW(0x2a0, 5, 0, 0x1ae, 0xc8, 0, 0, G(0x00579a84), 0, 0, 0xb, 0, 0, 0, 0);
    IC(0x268, 8, 0, 0x1b8, 0xdc, 0, 0, S_SPECULAR_OPTS, 0, S(0x2c), 0x15, 0, 0, 0, 0);
    xl(0x00579b50);
    IC(0x230, 5, 0, 0x1ed, 0xde, 0, 0, G(0x00579b54), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x1f8, 8, 0, 0x1b8, 0xf0, 0, 0, S_SPECULAR_OPTS, 0, S(0x30), 0x15, 0, 0, 0, 0);
    xl(0x005798c8);
    RAW(0x1c0, 5, 0, 0x1ed, 0xf2, 0, 0, G(0x005798cc), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x188, 8, 0, 0x1b8, 0x104, 0, 0, S_SPECULAR_OPTS, 0, S(0x34), 0x15, 0, 0, 0, 0);
    xl(0x005799b8);
    IC(0x150, 5, 0, 0x1ed, 0x106, 0, 0, G(0x005799bc), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x118, 8, 0, 0x1b8, 0x12c, 0, 0, S_SPECULAR_OPTS, 0, S(0x3c), 0x15, 0, 0, 0, 0);
    xl(0x00579b00);
    RAW(0xe0, 5, 0, 0x1ed, 0x12e, 0, 0, G(0x00579b04), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0xa8, 8, 0, 0x1b8, 0x118, 0, 0, S_MIRROR_OPTS, 0, S(0x44), 0x15, 0, 0, 0, 0);
    xl(0x00579ba8);
    IC(0x70, 5, 0, 0x1ed, 0x11a, 0, 0, G(0x00579bac), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x620));
    // the video modes (1..4), each added as it's found supported: two to a column, 0x14 apart
    int32_t x = 0x32;
    uint8_t second = 0;
    int32_t y = 0x96;
    if (ccall<uint8_t>(F_VidIsModeSupported, (int32_t)1)) {
        IC(0x690, 0xc, 0, 0x32, (uint32_t)y, 0, 0, 0x004f7e70, 1, S(0x18), 8, 0, 0, 0, 0);
        item_end(fr.at(0x658));
        y = 0xaa;
        add_items(self, fr.at(0x690));
        second = 1;
    }
    if (ccall<uint8_t>(F_VidIsModeSupported, (int32_t)2)) {
        IC(0x690, 0xc, 0, 0x32, (uint32_t)y, 0, 0, 0x004f7e64, 2, S(0x18), 8, 0, 0, 0, 0);
        item_end(fr.at(0x658));
        add_items(self, fr.at(0x690));
        if (second) { x = 0xc8; second = 0; y = 0x96; }
        else { second = 1; y += 0x14; }
    }
    if (ccall<uint8_t>(F_VidIsModeSupported, (int32_t)3)) {
        IC(0x690, 0xc, 0, (uint32_t)x, (uint32_t)y, 0, 0, 0x004f7e58, 3, S(0x18), 8, 0, 0, 0, 0);
        item_end(fr.at(0x658));
        add_items(self, fr.at(0x690));
        if (second) { second = 0; y = 0x96; x += y; }
        else { second = 1; y += 0x14; }
    }
    if (ccall<uint8_t>(F_VidIsModeSupported, (int32_t)4)) {
        IC(0x690, 0xc, 0, (uint32_t)x, (uint32_t)y, 0, 0, 0x004f7e4c, 4, S(0x18), 8, 0, 0, 0, 0);
        item_end(fr.at(0x658));
        add_items(self, fr.at(0x690));
    }
}
PORT_FN(0x00483220, "GfxOptionsControl::Added", Gfx_Added_n, fp_replay_items0)

static void __fastcall Gfx_Finalize_n(GfxOptionsControl* self, Edx) {
    const uint32_t dd = *(volatile uint32_t*)&self->draw_distance;
    const char* s = sec(S_SEC_GX);
    opt_set_f(s, 0x004f7e3c, dd);
    opt_set_f(s, 0x004f7e2c, *(volatile uint32_t*)&self->detail_level);
    opt_set_b(s, 0x004f7e20, self->sky);
    opt_set_b(s, 0x004f7e10, self->road);
    opt_set_b(s, 0x004f7e04, self->car);
    opt_set_b(s, 0x004f7df4, self->wall);
    opt_set_i(s, 0x004f7dec, self->smoke);
    opt_set_b(s, 0x004f7de0, self->filtering);
    opt_set_i(s, 0x004f7dd4, self->video_mode);
    opt_set_b(s, 0x004f7dd0, self->fog);
    opt_set_b(s, 0x004f7dc4, self->lighting);
    opt_set_i(s, 0x004f7db8, self->specular);
    opt_set_i(s, 0x004f7db0, self->mirror);
    opt_set_b(s, 0x004f7da8, self->mipmap);
    opt_set_i(s, 0x004f7da0, self->skids);
    opt_set_i(s, 0x004f7d98, self->shadow);
}
PORT_FN(0x00483eb0, "GfxOptionsControl::Finalize", Gfx_Finalize_n, fp_replay_options0)

// the Benchmark button: the Graphics tab's settings saved (MenuDoOptions's tab, through 0x4f72e0), bench.rpl replayed,
// its frames per second shown
static uint8_t __cdecl run_benchmark_n(int32_t) {
    GfxOptionsControl* g = UI_GP(GfxOptionsControl, S_GFX_OPTIONS);
    const char* s = sec(S_SEC_GX);
    opt_set_f(s, 0x004f7e3c, *(volatile uint32_t*)&g->draw_distance);
    opt_set_f(s, 0x004f7e2c, *(volatile uint32_t*)&g->detail_level);
    opt_set_b(s, 0x004f7e20, g->sky);
    opt_set_b(s, 0x004f7e10, g->road);
    opt_set_b(s, 0x004f7e04, g->car);
    opt_set_b(s, 0x004f7df4, g->wall);
    opt_set_i(s, 0x004f7dec, g->smoke);
    opt_set_b(s, 0x004f7de0, g->filtering);
    opt_set_i(s, 0x004f7dd4, g->video_mode);
    opt_set_b(s, 0x004f7dd0, g->fog);
    opt_set_b(s, 0x004f7dc4, g->lighting);
    opt_set_i(s, 0x004f7db8, g->specular);
    opt_set_i(s, 0x004f7db0, g->mirror);
    opt_set_b(s, 0x004f7da8, g->mipmap);
    opt_set_i(s, 0x004f7da0, g->skids);
    opt_set_i(s, 0x004f7d98, g->shadow);
    UI_GF(S_BENCH_FPS) = ccall<float>(F_ReplayBenchmark, (char*)0x004f7a18);
    return 0;
}
static void fp_run_benchmark(Footprint& f, int32_t) { f.replay_only = "writes the options, replays bench.rpl"; }
PORT_FN(0x00482080, "run_benchmark", run_benchmark_n, fp_run_benchmark)

// =====================================================================================================================
// SoundOptionsControl: the Sound tab
// =====================================================================================================================
// update_sound_strings (inlined three times): the quality's name and the fidelity's (8-bit / 16-bit) into the object.
// `strs` is the original's two locals, in its order; the fidelity (0 or 1, as the slider leaves it) picks one.
// FIX CANDIDATE: a fidelity outside 0..1 (an options file saying e.g. 2 is clamped to 1; a negative one to 1; only a value
// in (0, 1) truncates to 0) indexes past the two locals and copies whatever the stack holds there -- unreachable from the
// game's own options, which the constructor clamps
static __forceinline void update_strings_ctor(SoundOptionsControl* self) {
    const char* strs[2];
    if (!(UI_G8(S_SOUND_STR_ONCE) & 1)) {
        UI_G8(S_SOUND_STR_ONCE) = (uint8_t)(UI_G8(S_SOUND_STR_ONCE) | 1);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x005d5c50, (const char*)0x004f7e98);
        ccall<int>(F_atexit, (uint32_t)0x004849e0);
    }
    if (!(UI_G8(S_SOUND_STR_ONCE) & 2)) {
        UI_G8(S_SOUND_STR_ONCE) = (uint8_t)(UI_G8(S_SOUND_STR_ONCE) | 2);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x005d5c60, (const char*)0x004f7e84);
        ccall<int>(F_atexit, (uint32_t)0x004849d0);
    }
    xl(0x005d5c50);
    ((const char* volatile*)strs)[0] = UI_GP(const char, 0x005d5c54);
    xl(0x005d5c60);
    ((const char* volatile*)strs)[1] = UI_GP(const char, 0x005d5c64);
    const char* q = ccall<const char*>(F_MixerGetQualityString, x87_ftol(D(self->quality)));
    crt_strcpy((char*)self->quality_text, q);
    const int32_t k = x87_ftol(D(self->fidelity));
    crt_strcpy((char*)self->fidelity_text, ((const char* volatile*)strs)[k]);
}

static SoundOptionsControl* __fastcall Sound_ctor_n(SoundOptionsControl* self, Edx) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->widget = 0;
    tcall<MixerChooser*>(F_MixerChooser_ctor, (void*)&self->mixers, (int32_t*)&self->mixer, (uint32_t*)&self->grp_caps,
                         (uint32_t*)&self->grp_real);
    self->vtbl = (const void*)(uintptr_t)VT_SoundOptions;
    self->cd_audio = 1;
    {
        const float v = ccall<float>(F_MixerGetVolume);
        self->volume = v;
        self->volume_set = v;
    }
    const int32_t q = ccall<int32_t>(F_MixerGetQuality);
    *(volatile uint32_t*)&self->music_volume = 0x3f800000u;
    const char* s = sec(S_SEC_SOUND);
    self->spotter = 0;
    self->quality = (float)q;
    *(volatile uint32_t*)&self->fidelity = 0x3f800000u;
    opt_get_b(s, 0x004f7d8c, &self->cd_audio);
    opt_get_f(s, 0x004f7d7c, &self->music_volume);
    // FIX CANDIDATE: the fidelity is read into an uninitialised local; a missing option creates it from the stack's
    // garbage (then clamped to 0..1 below); the options file always has it after the first run
    volatile int32_t fid;
    opt_get_i(s, 0x004f7d70, &fid);
    self->fidelity = (float)fid;
    {
        const int32_t b = *(volatile int32_t*)&self->fidelity;
        if (b > 0x3f800000 || (uint32_t)b > 0x80000000u) *(volatile uint32_t*)&self->fidelity = 0x3f800000u;
    }
    opt_get_b(s, 0x004f7d68, &self->spotter);
    update_strings_ctor(self);
    self->restart = 0;
    return self;
}
PORT_FN(0x00484010, "SoundOptionsControl::SoundOptionsControl", Sound_ctor_n, fp_replay_options0)

static void __fastcall Sound_Added_n(SoundOptionsControl* self, Edx) {
    Frame<0x348> fr;
    once_xl(S_SOUND_ADDED_ONCE, 1, 0x005d5bf0, 0x004f7f20, 0x00484a40);
    once_xl(S_SOUND_ADDED_ONCE, 2, 0x005d5c00, 0x004f7f08, 0x00484a30);
    once_xl(S_SOUND_ADDED_ONCE, 4, 0x005d5c30, 0x004f7ef0, 0x00484a20);
    once_xl(S_SOUND_ADDED_ONCE, 8, 0x005d5c40, 0x004f7ed8, 0x00484a10);
    once_xl(S_SOUND_ADDED_ONCE, 0x10, 0x005d5c20, 0x004f7ec4, 0x00484a00);
    once_xl(S_SOUND_ADDED_ONCE, 0x20, 0x005d5c10, 0x004f7eac, 0x004849f0);
    xl(0x005d5c10);
    IC(0x348, 5, 0, 0x28, 0x82, 0, 0, G(0x005d5c14), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    IC(0x310, 1, 0, 0, 0, 0, 0, 0, 0, S(0x1c), 0, 0, 0, 0, 0);
    xl(0x005d5c20);
    IC(0x2d8, 5, 0, 0x32, 0x96, 0, 0, G(0x005d5c24), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    RAW(0x2a0, 0xf, 0, 0x96, 0x96, 0x64, 8, S_EMPTY, (uint32_t)-0xb, S(0x28), 0, 0, 0x3f800000, 0, 0);
    IC(0x268, 5, 0, 0x136, 0xaa, 0, 0, S(0x38), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    xl(0x005d5c30);
    IC(0x230, 5, 0, 0x32, 0xaa, 0, 0, G(0x005d5c34), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0x1f8, 0xf, 0, 0x96, 0xaa, 0x64, 8, S_EMPTY, 3, S(0x30), 0, 0, 0x40000000, 0, 0);
    IC(0x1c0, 1, 0, 0, 0, 0, 0, 0, 0, S(0x18), 0, 0, 0, 0, 0);
    xl(0x005d5c40);
    IC(0x188, 5, 0, 0x32, 0xbe, 0, 0, G(0x005d5c44), 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    RAW(0x150, 0xf, 0, 0x96, 0xbe, 0x64, 8, S_EMPTY, 2, S(0x34), 0, 0, 0x3f800000, 0, 0);
    IC(0x118, 5, 0, 0x136, 0xbe, 0, 0, S(0x58), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    IC(0xe0, 1, 1, 0, 0, 0, 0, 0, 0, S(0x18), 0, 0, 0, 0, 0);
    IC(0xa8, 1, 1, 0, 0, 0, 0, 0, 0, S(0x1c), 0, 0, 0, 0, 0);
    IC(0x70, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, S(0x80), 0, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    add_items(self, fr.at(0x348));
}
PORT_FN(0x004841f0, "SoundOptionsControl::Added", Sound_Added_n, fp_replay_items0)

static void __fastcall Sound_Create_n(SoundOptionsControl* self, Edx) {
    tcall<void>(F_UICC_AddNotification, self, (int32_t)0, (const void*)&self->fidelity, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)1, (const void*)&self->volume, (uint32_t)4);
    tcall<void>(F_UICC_AddNotification, self, (int32_t)2, (const void*)&self->quality, (uint32_t)4);
}
PORT_FN(0x004846e0, "SoundOptionsControl::Create", Sound_Create_n, fp_replay_notes0)

static void __fastcall Sound_Destroy_n(SoundOptionsControl* self, Edx) {
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->quality);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->fidelity);
    tcall<void>(F_UICC_RemoveNotification, self, (const void*)&self->volume);
}
PORT_FN(0x00484710, "SoundOptionsControl::Destroy", Sound_Destroy_n, fp_replay_notes0)

// the Callback's copy of update_sound_strings: the guard tested and set in memory (`test byte [g]; or byte [g]`)
static __forceinline void update_strings_cb(SoundOptionsControl* self) {
    const char* strs[2];
    if (!(UI_G8(S_SOUND_STR_ONCE) & 1)) {
        UI_G8(S_SOUND_STR_ONCE) = (uint8_t)(UI_G8(S_SOUND_STR_ONCE) | 1);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x005d5c50, (const char*)0x004f7e98);
        ccall<int>(F_atexit, (uint32_t)0x004849e0);
    }
    if (!(UI_G8(S_SOUND_STR_ONCE) & 2)) {
        UI_G8(S_SOUND_STR_ONCE) = (uint8_t)(UI_G8(S_SOUND_STR_ONCE) | 2);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x005d5c60, (const char*)0x004f7e84);
        ccall<int>(F_atexit, (uint32_t)0x004849d0);
    }
    xl(0x005d5c50);
    ((const char* volatile*)strs)[0] = UI_GP(const char, 0x005d5c54);
    xl(0x005d5c60);
    ((const char* volatile*)strs)[1] = UI_GP(const char, 0x005d5c64);
    const char* q = ccall<const char*>(F_MixerGetQualityString, x87_ftol(D(self->quality)));
    crt_strcpy((char*)self->quality_text, q);
    const int32_t k = x87_ftol(D(self->fidelity));
    crt_strcpy((char*)self->fidelity_text, ((const char* volatile*)strs)[k]);
}
// 0 the fidelity (the strings; the sound restarts when the tab closes), 1 the volume (set, and a test sound), 2 the
// quality (the strings, set)
static void __fastcall Sound_Callback_n(SoundOptionsControl* self, Edx, int32_t id, const void*) {
    if (id == 0) {
        update_strings_cb(self);
        self->restart = 1;
        return;
    }
    if (id == 1) {
        if (!(self->volume < self->volume_set || self->volume > self->volume_set)) return;
        const uint32_t v = *(volatile uint32_t*)&self->volume;
        *(volatile uint32_t*)&self->volume_set = v;
        ccall<void>(F_MixerSetVolume, v);
        ccall<void>(F_SoundToss, (const char*)0x004f7f38, (int32_t)0);
        return;
    }
    if (id == 2) {
        update_strings_cb(self);
        ccall<void>(F_MixerSetQuality, x87_ftol(D(self->quality)));
    }
}
static void fp_Sound_Callback(Footprint& f, SoundOptionsControl* self, Edx, int32_t id, const void*) {
    if (id == 1) { f.replay_only = "sets the mixer's volume, plays a test sound"; return; }
    if (id == 2) { f.replay_only = "sets the mixer's quality"; return; }
    if (id != 0) return;
    if ((UI_G8(S_SOUND_STR_ONCE) & 3) != 3) { f.replay_only = "constructs its Xlators (atexit)"; return; }
    f.add(self, sizeof(SoundOptionsControl), "this");
    f.add((void*)(uintptr_t)0x005d5c50, 0xc, "Xlator XL__8Bit");
    f.add((void*)(uintptr_t)0x005d5c60, 0xc, "Xlator XL__16Bit");
}
PORT_FN(0x00484740, "SoundOptionsControl::Callback", Sound_Callback_n, fp_Sound_Callback)

// =====================================================================================================================
// the controls' adjust dialog
// =====================================================================================================================
// get_control_tweaks: the sensitivities, ranges and speeds read from the options
static void __cdecl get_control_tweaks_n() {
    const char* s = sec(S_SEC_CONTROL);
    opt_get_f(s, 0x004f7bf4, (volatile void*)(uintptr_t)S_ADJ_THROTTLE_SENS);
    opt_get_f(s, 0x004f7c0c, (volatile void*)(uintptr_t)S_ADJ_THROTTLE_RANGE);
    opt_get_f(s, 0x004f7c1c, (volatile void*)(uintptr_t)S_ADJ_THROTTLE_SPEED);
    opt_get_b(s, 0x004f7c2c, (volatile void*)(uintptr_t)S_ADJ_NONLINEAR);
    opt_get_f(s, 0x004f7c40, (volatile void*)(uintptr_t)S_ADJ_STEER_SENS);
    opt_get_f(s, 0x004f7c54, (volatile void*)(uintptr_t)S_ADJ_STEER_RANGE);
    opt_get_f(s, 0x004f7c60, (volatile void*)(uintptr_t)S_ADJ_STEER_SPEED);
    opt_get_f(s, 0x004f7c6c, (volatile void*)(uintptr_t)S_ADJ_BRAKE_SENS);
    opt_get_f(s, 0x004f7c80, (volatile void*)(uintptr_t)S_ADJ_BRAKE_RANGE);
    opt_get_f(s, 0x004f7c8c, (volatile void*)(uintptr_t)S_ADJ_BRAKE_SPEED);
}
static void fp_none_options(Footprint& f) { f.replay_only = "reads or writes the options (OptionsGet creates a missing one)"; }
PORT_FN(0x00482e00, "get_control_tweaks", get_control_tweaks_n, fp_none_options)

// adjust_idle: the driver updated and read into the bars; the settings saved; the driver refreshed
static uint8_t __cdecl adjust_idle_n(int32_t*) {
    ccall<void>(F_DriverUpdate, ccall<float>(F_UIDeltaT));
    UI_GF(S_ADJ_THROTTLE) = ccall<float>(F_DriverGetThrottle);
    UI_GF(S_ADJ_BRAKE) = ccall<float>(F_DriverGetBraking);
    UI_GF(S_ADJ_STEER) = ccall<float>(F_DriverGetSteering, 0.0f);
    const char* s = sec(S_SEC_CONTROL);
    opt_set_f(s, 0x004f7b50, G(S_ADJ_THROTTLE_SENS));
    opt_set_f(s, 0x004f7b68, G(S_ADJ_THROTTLE_RANGE));
    opt_set_f(s, 0x004f7b78, G(S_ADJ_THROTTLE_SPEED));
    opt_set_b(s, 0x004f7b88, UI_G8(S_ADJ_NONLINEAR));
    opt_set_f(s, 0x004f7b9c, G(S_ADJ_STEER_SENS));
    opt_set_f(s, 0x004f7bb0, G(S_ADJ_STEER_RANGE));
    opt_set_f(s, 0x004f7bbc, G(S_ADJ_STEER_SPEED));
    opt_set_f(s, 0x004f7bc8, G(S_ADJ_BRAKE_SENS));
    opt_set_f(s, 0x004f7bdc, G(S_ADJ_BRAKE_RANGE));
    opt_set_f(s, 0x004f7be8, G(S_ADJ_BRAKE_SPEED));
    ccall<void>(F_DriverRefresh);
    return 0;
}
static void fp_adjust_idle(Footprint& f, int32_t*) { f.replay_only = "polls the input devices (DriverUpdate), writes the options"; }
PORT_FN(0x00482ce0, "adjust_idle", adjust_idle_n, fp_adjust_idle)

// tune_default: "restore the defaults?" -- tune.def loaded, the settings read again
static uint8_t __cdecl tune_default_n(int32_t) {
    uint8_t g = UI_G8(S_TUNE_ONCE);
    if (!(g & 1)) {
        UI_G8(S_TUNE_ONCE) = (uint8_t)(g | 1);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579bc8, (const char*)0x004f7c98);
        ccall<int>(F_atexit, (uint32_t)0x00482fa0);
        g = UI_G8(S_TUNE_ONCE);
    }
    if (!(g & 2)) {
        UI_G8(S_TUNE_ONCE) = (uint8_t)(g | 2);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579d48, (const char*)0x004f7cc4);
        ccall<int>(F_atexit, (uint32_t)0x00482f90);
    }
    xl(0x00579bc8);
    xl(0x00579d48);
    if (ccall<uint8_t>(F_UIDoYesNoBox, UI_GP(const char, 0x00579d4c), UI_GP(const char, 0x00579bcc), (uint32_t)0)) {
        ccall<void>(F_OptionsLoadModule, (const char*)0x004f7ce0);
        ccall<void>(F_get_control_tweaks);
    }
    return 0;
}
static void fp_tune_default(Footprint& f, int32_t) { f.replay_only = "a yes / no box (modal), loads tune.def"; }
PORT_FN(0x00482ed0, "tune_default", tune_default_n, fp_tune_default)

// the Tune button: the bindings saved, the tweaks reset and read, a dialog of the three bars with their sensitivity,
// range and speed sliders (a digital control gets a speed slider for its sensitivity, and no range steps), the
// non-linear steering box, Defaults (tune_default) and Back; adjust_idle runs it
static uint8_t __cdecl adjust_ctls_n(int32_t) {
    Frame<0x538> fr;
    tcall<void>(F_Controls_Finalize, UI_GP(void, S_CONTROLS_OPTIONS));
    MO_G32(S_ADJ_834) = 0xffffffffu;
    MO_G32(S_ADJ_THROTTLE_SENS) = 0x3f800000u;
    MO_G32(S_ADJ_THROTTLE_RANGE) = 0x3f800000u;
    MO_G32(S_ADJ_THROTTLE_SPEED) = 0x3f800000u;
    MO_G32(S_ADJ_STEER_SENS) = 0x3f800000u;
    MO_G32(S_ADJ_STEER_RANGE) = 0x3f800000u;
    MO_G32(S_ADJ_STEER_SPEED) = 0x3f800000u;
    MO_G32(S_ADJ_BRAKE_SENS) = 0x3f800000u;
    MO_G32(S_ADJ_BRAKE_RANGE) = 0x3f800000u;
    MO_G8(S_ADJ_NONLINEAR) = 1;
    MO_G32(S_ADJ_BRAKE_SPEED) = 0x3f800000u;
    ccall<void>(F_get_control_tweaks);
    // the three bars, on the frame (their x, y, w, h are _UIAddItems's to set)
    fr.d(0x510) = 0;
    fr.d(0x524) = VT_Meter;
    fr.d(0x4f4) = 0;
    fr.d(0x508) = VT_Meter;
    fr.d(0x50c) = S_ADJ_THROTTLE;
    fr.d(0x4f0) = S_ADJ_BRAKE;
    fr.d(0x4d4) = S_ADJ_STEER;
    fr.d(0x4ec) = VT_SteerMeter;
    fr.d(0x4d8) = 0;
    {
        uint8_t g = UI_G8(S_ADJUST_ONCE);
        static const uint32_t xls[8][3] = {
            {0x00579a20, 0x004f7a24, 0x00483040}, {0x00579bd8, 0x004f7a3c, 0x00483030}, {0x00579b10, 0x004f7a54, 0x00483020},
            {0x00579c80, 0x004f7a7c, 0x00483010}, {0x00579c10, 0x004f7a9c, 0x00483000}, {0x00579c58, 0x004f7ab8, 0x00482ff0},
            {0x00579ac0, 0x004f7ad4, 0x00482fe0}, {0x00579bf8, 0x004f7af8, 0x00482fd0}};
        for (int k = 0; k < 8; k++) {
            const uint8_t bit = (uint8_t)(1u << k);
            if (!(g & bit)) {
                UI_G8(S_ADJUST_ONCE) = (uint8_t)(g | bit);
                tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)xls[k][0], (const char*)(uintptr_t)xls[k][1]);
                ccall<int>(F_atexit, xls[k][2]);
                g = UI_G8(S_ADJUST_ONCE);
            }
        }
        uint8_t g2 = UI_G8(S_ADJUST_ONCE2);
        if (!(g2 & 1)) {
            UI_G8(S_ADJUST_ONCE2) = (uint8_t)(g2 | 1);
            tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579938, (const char*)0x004f7b14);
            ccall<int>(F_atexit, (uint32_t)0x00482fc0);
            g2 = UI_G8(S_ADJUST_ONCE2);
        }
        if (!(g2 & 2)) {
            UI_G8(S_ADJUST_ONCE2) = (uint8_t)(g2 | 2);
            tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579a90, (const char*)0x004f7b34);
            ccall<int>(F_atexit, (uint32_t)0x00482fb0);
        }
    }
    uint32_t t, p;
    // throttle
    xl(0x00579a20);
    IC(0x4d0, 5, 0, 0x32, 0xa5, 0, 0, G(0x00579a24), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    IC(0x498, 0x17, 0, 0xa5, 0xa5, 0x32, 0xa, S_EMPTY, 0, U(fr.at(0x524)), 0, 0, 0, 0, 0);
    if (ccall<uint8_t>(F_DriverIsThrottleDigital)) { xl(0x00579c58); t = G(0x00579c5c); }
    else { xl(0x00579ac0); t = G(0x00579ac4); }
    IC(0x460, 5, 0, 0x37, 0xb9, 0, 0, t, 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    p = ccall<uint8_t>(F_DriverIsThrottleDigital) ? S_ADJ_THROTTLE_SPEED : S_ADJ_THROTTLE_SENS;
    IC(0x428, 0xf, 0, 0xa0, 0xb9, 0x64, 8, S_EMPTY, 0x14, p, 0, 0x3d4ccccd, 0x3f800000, 0, 0);
    if (ccall<uint8_t>(F_DriverIsThrottleDigital)) t = 0x004f7b3c;
    else { xl(0x00579c10); t = G(0x00579c14); }
    IC(0x3f0, 5, 0, 0x37, 0xcd, 0, 0, t, 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    p = ccall<uint8_t>(F_DriverIsThrottleDigital) < 1 ? 0x14u : 0u;
    RAW(0x3b8, 0xf, 0, 0xa0, 0xcd, 0x64, 8, S_EMPTY, p, S_ADJ_THROTTLE_RANGE, 0, 0x3f000000, 0x40200000, 0, 0);
    // brake
    xl(0x00579bd8);
    IC(0x380, 5, 0, 0x32, 0x109, 0, 0, G(0x00579bdc), 0, 0, sty1(S_COL_BF4), 0, 0, 0, 0);
    IC(0x348, 0x17, 0, 0xa5, 0x109, 0x32, 0xa, S_EMPTY, 0, U(fr.at(0x508)), 0, 0, 0, 0, 0);
    if (ccall<uint8_t>(F_DriverIsBrakeDigital)) { xl(0x00579c58); t = G(0x00579c5c); }
    else { xl(0x00579ac0); t = G(0x00579ac4); }
    IC(0x310, 5, 0, 0x37, 0x11d, 0, 0, t, 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    p = ccall<uint8_t>(F_DriverIsBrakeDigital) ? S_ADJ_BRAKE_SPEED : S_ADJ_BRAKE_SENS;
    IC(0x2d8, 0xf, 0, 0xa0, 0x11d, 0x64, 8, S_EMPTY, 0x14, p, 0, 0x3d4ccccd, 0x3f800000, 0, 0);
    if (ccall<uint8_t>(F_DriverIsBrakeDigital)) t = 0x004f7b40;
    else { xl(0x00579c10); t = G(0x00579c14); }
    IC(0x2a0, 5, 0, 0x37, 0x131, 0, 0, t, 0, 0, sty(S_COL_BF4, S_COL_974), 0, 0, 0, 0);
    p = ccall<uint8_t>(F_DriverIsBrakeDigital) < 1 ? 0x14u : 0u;
    RAW(0x268, 0xf, 0, 0xa0, 0x131, 0x64, 8, S_EMPTY, p, S_ADJ_BRAKE_RANGE, 0, 0x3f000000, 0x40200000, 0, 0);
    // steering
    xl(0x00579c80);
    IC(0x230, 5, 0, 0x136, 0xa5, 0, 0, G(0x00579c84), 0, 0, 0xb, 0, 0, 0, 0);
    IC(0x1f8, 0x17, 0, 0x1a9, 0xa5, 0x64, 0xa, S_EMPTY, 0, U(fr.at(0x4ec)), 0, 0, 0, 0, 0);
    if (ccall<uint8_t>(F_DriverIsSteeringDigital)) { xl(0x00579c58); t = G(0x00579c5c); }
    else { xl(0x00579ac0); t = G(0x00579ac4); }
    RAW(0x1c0, 5, 0, 0x13b, 0xb9, 0, 0, t, 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    p = ccall<uint8_t>(F_DriverIsSteeringDigital) ? S_ADJ_STEER_SPEED : S_ADJ_STEER_SENS;
    IC(0x188, 0xf, 0, 0x1c2, 0xb9, 0x64, 8, S_EMPTY, 0x14, p, 0, 0x3d4ccccd, 0x3f800000, 0, 0);
    xl(0x00579c10);
    RAW(0x150, 5, 0, 0x13b, 0xcd, 0, 0, G(0x00579c14), 0, 0, sty(S_COL_974, S_COL_BF4), 0, 0, 0, 0);
    RAW(0x118, 0xf, 0, 0x1c2, 0xcd, 0x64, 8, S_EMPTY, 0x14, S_ADJ_STEER_RANGE, 0, 0x3f000000, 0x40200000, 0, 0);
    xl(0x00579b10);
    IC(0xe0, 0xb, 0, 0x13b, 0xe1, 0, 0, G(0x00579b14), 0, S_ADJ_NONLINEAR, 7, 0, 0, 0, 0);
    xl(0x00579938);
    IC(0xa8, 2, 0, 0x1e5, 0x1a5, 0, 0, G(0x0057993c), 0, F_tune_default, 2, 0, 0, 0, 0);
    xl(0x00579878);
    IC(0x70, 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, G(0x0057987c), 0, 0, 6, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    xl(0x00579bf8);
    fr.d(0x538) = G(0x00579bfc);
    fr.d(0x52c) = U(fr.at(0x4d0));
    fr.d(0x534) = 0x004f7b44;
    fr.d(0x530) = 0xfffffffeu;
    fr.d(0x528) = F_adjust_idle;
    ccall<int32_t>(F_UIDoDialog, fr.at(0x538), UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), (int32_t)-999, (int32_t)-999, (int32_t)1);
    return 0;
}
static void fp_adjust_ctls(Footprint& f, int32_t) { f.replay_only = "runs a dialog (modal), writes the options"; }
PORT_FN(0x004821e0, "adjust_ctls", adjust_ctls_n, fp_adjust_ctls)

// =====================================================================================================================
// MenuDoOptions
// =====================================================================================================================
// The Options screen. Its frame, as the original's (entry esp - n):
//   0x770 the tab chosen (options_menu; 0x20, the hacks, only while they're enabled)
//   0x76c / 0x768 / 0x764 / 0x760 / 0x75c the groups of Hacks / Graphics / Sound / Controls / Driving Aids
//   0x758 the UIDialog; 0x744 Aids, 0x71c Gfx, 0x6d4 Hack, 0x68c Sound, 0x5e4 Controls; 0x540 the items (24)
static void __cdecl MenuDoOptions_n() {
    Frame<0x770> fr;
    const char* global = sec(S_SEC_GLOBAL);
    fr.d(0x770) = 1;
    ccall<void>(F_OptionsGetI, global, (const char*)0x004f79dc, fr.at(0x770));
    if (!ccall<uint8_t>(F_HackEnabled) && fr.d(0x770) == 0x20) fr.d(0x770) = 1;
    tcall<void*>(F_Sound_ctor, fr.at(0x68c));
    tcall<void*>(F_Hack_ctor, fr.at(0x6d4));
    tcall<void*>(F_Gfx_ctor, fr.at(0x71c));
    UI_GP(void, S_GFX_OPTIONS) = fr.at(0x71c);
    tcall<void*>(F_Controls_ctor, fr.at(0x5e4));
    UI_GP(void, S_CONTROLS_OPTIONS) = fr.at(0x5e4);
    tcall<void*>(F_Aids_ctor, fr.at(0x744));
    {
        const uint8_t g = UI_G8(S_OPTIONS_ONCE);
        if (!(g & 1)) {
            UI_G8(S_OPTIONS_ONCE) = (uint8_t)(g | 1);
            tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)0x00579968, (const char*)0x004f79ec);
            ccall<int>(F_atexit, (uint32_t)0x004821d0);
        }
    }
    const uint32_t tab = U(fr.at(0x770));
    IC(0x540, 1, 0, 0, 0, 0, 0, 0, 0, U(fr.at(0x764)), 0, 0, 0, 0, 0);
    IC(0x508, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(fr.at(0x68c)), 0, 0, 0, 0, 0);
    IC(0x4d0, 1, 1, 0, 0, 0, 0, 0, 0, U(fr.at(0x764)), 0, 0, 0, 0, 0);
    IC(0x498, 1, 0, 0, 0, 0, 0, 0, 0, U(fr.at(0x760)), 0, 0, 0, 0, 0);
    IC(0x460, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(fr.at(0x5e4)), 0, 0, 0, 0, 0);
    IC(0x428, 1, 1, 0, 0, 0, 0, 0, 0, U(fr.at(0x760)), 0, 0, 0, 0, 0);
    IC(0x3f0, 1, 0, 0, 0, 0, 0, 0, 0, U(fr.at(0x75c)), 0, 0, 0, 0, 0);
    IC(0x3b8, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(fr.at(0x744)), 0, 0, 0, 0, 0);
    IC(0x380, 1, 1, 0, 0, 0, 0, 0, 0, U(fr.at(0x75c)), 0, 0, 0, 0, 0);
    IC(0x348, 1, 0, 0, 0, 0, 0, 0, 0, U(fr.at(0x768)), 0, 0, 0, 0, 0);
    IC(0x310, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(fr.at(0x71c)), 0, 0, 0, 0, 0);
    IC(0x2d8, 1, 1, 0, 0, 0, 0, 0, 0, U(fr.at(0x768)), 0, 0, 0, 0, 0);
    IC(0x2a0, 1, 0, 0, 0, 0, 0, 0, 0, U(fr.at(0x76c)), 0, 0, 0, 0, 0);
    IC(0x268, 0x17, 0, 0, 0, 0, 0, S_EMPTY, 0, U(fr.at(0x6d4)), 0, 0, 0, 0, 0);
    IC(0x230, 1, 1, 0, 0, 0, 0, 0, 0, U(fr.at(0x76c)), 0, 0, 0, 0, 0);
    IC(0x1f8, 1, 0, 0, 0, 0, 0, 0, 0, S_HACK_GROUP, 0, 0, 0, 0, 0);
    xl(0x00579928);
    IC(0x1c0, 0xc, 0, 0x17f, 0x46, (uint32_t)-1, 0, G(0x0057992c), U(fr.at(0x76c)), tab, 3, 0, 0, 0, 0);
    IC(0x188, 1, 1, 0, 0, 0, 0, 0, 0, S_HACK_GROUP, 0, 0, 0, 0, 0);
    xl(0x00579ce8);
    IC(0x150, 0xc, 0, 0x21, 0x58, (uint32_t)-1, 0, G(0x00579cec), U(fr.at(0x768)), tab, 3, 0, 0, 0, 0);
    xl(0x00579ad0);
    IC(0x118, 0xc, 0, 0xad, 0x58, (uint32_t)-1, 0, G(0x00579ad4), U(fr.at(0x764)), tab, 3, 0, 0, 0, 0);
    xl(0x00579a40);
    IC(0xe0, 0xc, 0, 0x139, 0x58, (uint32_t)-1, 0, G(0x00579a44), U(fr.at(0x760)), tab, 3, 0, 0, 0, 0);
    xl(0x00579868);
    IC(0xa8, 0xc, 0, 0x1c5, 0x58, (uint32_t)-1, 0, G(0x0057986c), U(fr.at(0x75c)), tab, 3, 0, 0, 0, 0);
    xl(0x00579878);
    IC(0x70, 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, G(0x0057987c), 0, 0, 6, 0, 0, 0, 0);
    item_end(fr.at(0x38));
    xl(0x00579968);
    fr.d(0x758) = G(0x0057996c);
    fr.d(0x74c) = U(fr.at(0x540));
    fr.d(0x754) = 0x004f79fc;
    fr.d(0x750) = 0xfffffffeu;
    fr.d(0x748) = 0;
    ccall<int32_t>(F_UIDoDialog, fr.at(0x758), UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), (int32_t)-999, (int32_t)-999, (int32_t)1);
    ccall<void>(F_OptionsSetI, global, (const char*)0x004f7a08, (int32_t)fr.d(0x770));
    SoundOptionsControl* snd = (SoundOptionsControl*)fr.at(0x68c);
    const char* sound = sec(S_SEC_SOUND);
    opt_set_b(sound, 0x004f7d8c, snd->cd_audio);
    opt_set_i(sound, 0x004f7d70, x87_ftol(D(snd->fidelity)));
    opt_set_f(sound, 0x004f7d7c, *(volatile uint32_t*)&snd->music_volume);
    opt_set_b(sound, 0x004f7d68, snd->spotter);
    if (snd->restart) ccall<void>(F_SoundRestart);
    tcall<void>(F_Hack_Finalize, fr.at(0x6d4));
    tcall<void>(F_Gfx_Finalize, fr.at(0x71c));
    tcall<void>(F_Controls_Finalize, fr.at(0x5e4));
    tcall<void>(F_Aids_Finalize, fr.at(0x744));
    ccall<void>(F_OptionsFlush);
    // the destructors, inlined: the vtables put back (Aids, Controls, Gfx; the test panel's own destructor; the car
    // list's)
    fr.d(0x744) = VT_UICustomControl;
    tcall<void>(F_CTC_dtor, fr.at(0x578));
    fr.d(0x5e4) = VT_UICustomControl;
    fr.d(0x71c) = VT_UICustomControl;
    tcall<void>(F_UIStringList_dtor, fr.at(0x6ac));
    // FIX CANDIDATE: 0x4f72e0 and 0x579b0c are left aimed at this frame (the Graphics and Controls tabs); only
    // run_benchmark and adjust_ctls read them, and only while the dialog runs
}
static void fp_MenuDoOptions(Footprint& f) { f.replay_only = "the Options screen: runs a dialog (modal), reads and writes the options"; }
PORT_FN(0x004819a0, "MenuDoOptions", MenuDoOptions_n, fp_MenuDoOptions)

#undef RAW
#undef IC
#undef S
#undef G
#undef D
}  // namespace menu_options
}  // namespace
