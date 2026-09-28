// menu_car.cpp -- M3 UI stage, step U2 (group A): the garage setup editor, mcar.obj, rewritten faithfully (library `menu`).
//
//   MenuEditCar (the garage: the car file and its setup loaded, the setup set of 8 slots, a full-screen dialog of five
//   custom controls on radio-button pages, the setup saved and exported as HTML on the way out), file_cb (the File
//   dialog: FileControl), save_setup_dlg / load_setup_dlg (a slot picked from the set, a name typed), save_setup_cb /
//   load_setup_cb / default_setup_cb (the File dialog's buttons: an unsaved setup offered for saving first),
//   load_setup_set / save_setup_set (<user>\setups\<dir>.set, or an old <user><car>.set imported), export_setup
//   (<user>setups\<dir>.html), the graph callbacks tire_fn / shock_fn / power_fn, get_gear; the controls:
//   BalanceControl (bars of the car's balance), ChassisControl (springs, shocks, anti-roll: numbers with arrows, and a
//   shock graph), AlignControl (camber, toe, ride height, brake bias, lock: pictures and a tire graph), TorqueCurveControl
//   (the engine's power and torque curves), DrivetrainControl (gear ratios, final drive, the speed in each gear),
//   AeroControl (spoilers; the aero kit's groups), FileControl (the setup's summary, Load / Save / Default) -- each one's
//   Create, Added, Callback, Draw and deleting destructor; and the 43 function-local Xlators' destructor helpers (the
//   functions the draws and Added register with atexit when they build their Xlators: bare `ret`s, an Xlator's
//   destructor being empty).
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this object's own too),
// virtual calls through the vtable, the compiler's inline string code as it runs it (ui_types.h). The dialog item
// lists are built on the stack as the original builds them -- UIDialogItem's constructor (0x4091c0) called where the
// original calls it, the items the compiler inlined stored field by field -- in the same order, with the same values,
// the translated texts read after their Xlator is refreshed. x87: a register value is a double, a stored one a float,
// the original's grouping and constants' widths, `fst` (a float stored, the register kept) kept apart from `fstp`,
// fsin / fcos / fptan in asm helpers that finish what the original does with the full-precision result, __ftol as
// x87_ftol, comparisons with the original's NaN outcome -- including the places the compiler tested a float's bits as an
// integer (`cmp dword [t], 0x34000000`) where it had just compared the register (docs/PORTING.md).
//
// Footprints (main thread). replay_only: MenuEditCar and every dialog (modal: their own input loops), the setup-set
// and export functions (files), Create (notification copies: allocates), Added (builds widgets), the destructors
// (free), and a Draw the first time it runs (it builds its function-local Xlators: atexit); AlignControl::Draw on its
// tire page (TireCreate / TireDestroy). The draws write the canvas they're given (its clip and pixels), the
// current-canvas static and their Xlators; BalanceControl::Draw the CarData (CarFileCombine), DrivetrainControl::Draw
// the setup's gear ratios (kept 0.04 apart), AeroControl::Draw the running window's groups. Callback writes the
// control and its CustomWidget. get_gear, tire_fn, shock_fn, power_fn write nothing but their own frames.
//
// Fixes (// FIX:, docs/FIXES.md "Menus"): MenuEditCar builds "<car>.car" in 240 bytes, not 32 (a car name over 27
// characters was overwritten by the item list before the resource set was unloaded), and cuts a name too long for its
// two 240-byte buffers; it keeps the '*' it puts before an unsaved setup's name inside the name's 64 bytes (a 62- or
// 63-character name was moved past the setup); the save dialog's name buffer is the field's 64 bytes and the setup's
// name is copied into it bounded; DrivetrainControl::Draw keeps only the setup's six ratios apart (a car file with more
// gears read and wrote past them). Every other input gives the original's bits.
//
// FIX CANDIDATEs (left faithful, marked where they are): load_setup_set / save_setup_set / export_setup's 256-byte
// paths (a long user directory or name), a slot outside 0..7 (from a damaged .set) indexing past the set, ChassisControl
// / AlignControl's page outside their text tables, and the sprintf buffers a huge value would overrun.
#include <math.h>
#include <stdint.h>
#include <initializer_list>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "x87.h"
#include "menu_car.h"

namespace {
namespace menu_car {
using namespace uit;
using namespace mcar;

#define D(x) ((double)(x))
#define MF(a) UI_GF(a)                                   // a float static, read where the original reads it
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t f_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static __forceinline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

// the float constants the original multiplies by (their exact bits)
static const uint32_t K_EPS = 0x34000000u;               // FLT_EPSILON
#define KF(bits) D(bits_f(bits))

// ---- statics the compiler builds on first use: `test byte [guard], bit; jne; or [guard], bit; Xlator(key); atexit` ----
static __forceinline void xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(UI_G8(guard) & bit)) {
        UI_G8(guard) = (uint8_t)(UI_G8(guard) | bit);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)xl, (const char*)(uintptr_t)key);
        ccall<int>(F_atexit, dtor);
    }
}
static __forceinline const char* xl_text(uint32_t xl) { return xlate(xl); }

// ---- dialog items (UIDialogItem, 14 dwords) --------------------------------------------------------------------------
// the constructor, called where the original calls it (float fields as the bits the original pushes)
static __forceinline void it_ctor(UIDialogItem* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  uint32_t text, uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi,
                                  uint32_t sel, uint32_t s34) {
    tcall<void*>(F_UIDialogItem_ctor, (void*)it, type, id, x, y, w, h, text, i1c, data, style, lo, hi, sel, s34);
}
// ... an item the compiler inlined: its 14 dwords stored
static __forceinline void it_set(UIDialogItem* it, uint32_t type, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 uint32_t text, uint32_t i1c, uint32_t data, uint32_t style, uint32_t lo, uint32_t hi,
                                 uint32_t sel, uint32_t s34) {
    volatile uint32_t* d = (volatile uint32_t*)it;
    d[0] = type; d[1] = id; d[2] = x; d[3] = y; d[4] = w; d[5] = h; d[6] = text;
    d[7] = i1c; d[8] = data; d[9] = style; d[10] = lo; d[11] = hi; d[12] = sel; d[13] = s34;
}
// the end of a list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void it_end(UIDialogItem* it) { crt_copy(it, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline uint32_t P(const volatile void* p) { return (uint32_t)(uintptr_t)p; }
static __forceinline void dialog(UIDialog* d, const char* title, uint32_t bg, int32_t def, UIDialogItem* items) {
    volatile uint32_t* v = (volatile uint32_t*)d;
    v[0] = P(title);
    v[1] = bg;
    v[2] = (uint32_t)def;
    v[3] = P(items);
    v[4] = 0;
}
enum : uint32_t { T_EMPTY = 0x004e4db8, T_F10 = 0x004f9ec4, T_DASH = 0x004f9ec0, K_100 = 0x42c80000u, K_1 = 0x3f800000u };

// the label style: 0xb when the two text colours are the same, else 0xc (`sub; cmp eax, 1; sbb eax, eax; add eax, 0xc`)
static __forceinline uint32_t label_style(uint32_t a, uint32_t b) { return UI_GU32(a) - UI_GU32(b) == 0 ? 0xbu : 0xcu; }
static __forceinline uint32_t lstyle() { return label_style(M_C_57B6AC, M_C_57AEDC); }
static __forceinline uint32_t lstyle_r() { return label_style(M_C_57AEDC, M_C_57B6AC); }

// |a - b| in the register (fld a; fsub b; fabs), and the two tests the original makes of it
static __forceinline double absdiff(uint32_t a, uint32_t b) { return fabs(D(MF(a)) - D(MF(b))); }
// fcom(p) eps; test ah, 0x41: not above epsilon (close, or NaN)
static __forceinline bool near_reg(double d) { return !(d > KF(K_EPS)); }
// fstp dword [t] ... cmp dword [t], 0x34000000; jle: the stored float's bits, as a signed int, not above epsilon's
static __forceinline bool near_bits(double d) {
    volatile float f = (float)d;
    int32_t b;
    memcpy(&b, (const void*)&f, 4);
    return b <= (int32_t)K_EPS;
}

// ---- the setup (CarSetup, 0xd4) and the set -------------------------------------------------------------------------
static __forceinline void copy_setup(uint32_t dst, uint32_t src) { crt_copy((void*)(uintptr_t)dst, (const void*)(uintptr_t)src, 0xd4); }
// set->setups[slot] (lea: slot * 0xd4 + 0x57a7ec, 32 bits)
static __forceinline uint32_t slot_addr(int32_t slot) { return (uint32_t)slot * 0xd4u + M_SET_SETUPS; }
// repe cmpsb over the setup and its loaded copy: nonzero if they differ
static __forceinline int setup_changed() {
    return crt_memcmp_ne((const void*)(uintptr_t)M_SETUP, (const void*)(uintptr_t)M_SETUP_ORIG, 0xd4);
}

// =====================================================================================================================
// the graph callbacks, get_gear
// =====================================================================================================================
// sin(x * pi/180) as fsin gives it, stored to a float (fld; fmul dword; fsin; fstp dword)
static __forceinline float sin_deg_f(float x) {
    float r;
    static const uint32_t k = 0x3c8efa35u;               // 0.017453292f
    __asm { fld x
            fmul dword ptr k
            fsin
            fstp r }
    return r;
}
// tan(x * pi/180) as fptan gives it (the 1.0 it pushes popped), stored to a float
static __forceinline float tan_deg_f(float x) {
    float r;
    static const uint32_t k = 0x3c8efa35u;
    __asm { fld x
            fmul dword ptr k
            fptan
            fstp st(0)
            fstp r }
    return r;
}
// the tire graph's point: the lateral force at a slip angle (degrees), for a load and a camber
struct TireArg { void* volatile tire; volatile float load; volatile float camber; };
static double __cdecl tire_fn_n(float x, void* pv) {
    TireArg* a = (TireArg*)pv;
    if (a->tire == 0) return KF(0x00000000u);
    volatile float fz = 0.0f;                            // [esp]: GetForce's last out (mov dword [esp], 0)
    volatile float out[3];                               // [esp+4]: the force (P3DBase)
    *(volatile uint32_t*)&fz = 0;
    const float s = sin_deg_f(a->camber);
    const float t = tan_deg_f(x);
    const float l = (float)(D(a->load) * KF(0x408e6666u));
    typedef void(__fastcall * GetForce_t)(void*, Edx, volatile float*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, volatile float*);
    ((GetForce_t)(uintptr_t)F_Tire_GetForce)(a->tire, 0, out, f_bits(l), f_bits(t), f_bits(s), 0u, K_1, &fz);
    return fabs(D(out[0])) * KF(0x3e661cc4u);
}
static void fp_none_fn(Footprint&, float, void*) {}
PORT_FN(0x00493160, "tire_fn", tire_fn_n, fp_none_fn)

// the shock graph's point: a damper set up from the four rates (scaled by 4.45 / 0.0254, computed at run time), its
// force at a speed
static double __cdecl shock_fn_n(float x, void* pv) {
    const volatile float* p = (const volatile float*)pv;
    volatile float c0254 = bits_f(0x3cd013a9u), c1 = bits_f(K_1), c445 = bits_f(0x408e6666u);
    double k = D(c0254) / D(c1);
    k = D(c445) / k;
    const float a3 = (float)(D(p[3]) * k);
    const float a2 = (float)(D(p[2]) * k);
    const float a1 = (float)(D(p[1]) * k);
    const float a0 = (float)(k * D(p[0]));
    volatile uint32_t damper[4];                         // Damper (16 bytes), on the stack
    typedef void(__fastcall * Setup_t)(volatile uint32_t*, Edx, uint32_t, uint32_t, uint32_t, uint32_t);
    ((Setup_t)(uintptr_t)F_Damper_Setup)(damper, 0, f_bits(a0), f_bits(a1), f_bits(a2), f_bits(a3));
    const float v = (float)(D(x) * KF(0x3cd013a9u));
    typedef double(__fastcall * Rate_t)(volatile uint32_t*, Edx, uint32_t);
    const double r = ((Rate_t)(uintptr_t)F_Damper_GetDampingRate)(damper, 0, f_bits(v));
    return ((r * D(x)) * KF(0x3cd013a9u)) * KF(0x3e661cc4u);
}
PORT_FN(0x004931f0, "shock_fn", shock_fn_n, fp_none_fn)

// a gear's ratio: the car file's, or the setup's if the car file has none (its first is 0)
static float* __cdecl get_gear_n(int32_t i) {
    if (fabs(D(MF(0x0057b120))) > KF(K_EPS)) return (float*)(uintptr_t)((uint32_t)i * 4u + 0x0057b11cu);
    return (float*)(uintptr_t)((uint32_t)i * 4u + 0x0057afacu);
}
static void fp_none_i(Footprint&, int32_t) {}
PORT_FN(0x00493c40, "get_gear", get_gear_n, fp_none_i)

// the torque graph's point: a PowerCurve's power (flag set) or torque at an rpm scaled by +0x10, 0 past the limit at +8
struct PowerArg { void* volatile curve; volatile uint8_t power; uint8_t _5[3]; volatile float limit, scale, rpm_scale; };
static double __cdecl power_fn_n(float x, void* pv) {
    PowerArg* a = (PowerArg*)pv;
    const double prod = D(a->rpm_scale) * D(x);
    volatile float r = (float)prod;
    if (prod > D(a->limit)) return KF(0x00000000u);
    const volatile float* c = (const volatile float*)a->curve;
    if (a->power != 0) {
        const double t1 = (D(c[1]) - D(c[3])) * D(r);
        double t2 = D(r) - D(c[4]);
        t2 = !(0.0 >= t2) ? t2 : 0.0;
        t2 = t2 * D(c[5]);
        const double s = t1 - t2;
        const double t3 = (D(r) * D(r)) * D(c[0]);
        return (((s + t3) + D(c[2])) * D(a->scale)) * D(a->rpm_scale) * KF(0x3f3c9de0u);
    }
    double t2 = D(r) - D(c[4]);
    t2 = !(0.0 >= t2) ? t2 : 0.0;
    t2 = (t2 * D(c[5])) * KF(0xbf3c9de0u);
    const double u = ((D(c[1]) - D(c[3])) * D(r)) * KF(0x3f3c9de0u);
    const double v = ((D(r) * D(r)) * D(c[0])) * KF(0x3f3c9de0u);
    const double w = D(c[2]) * KF(0x3f3c9de0u);
    return ((((t2 + u) + v) + w) * D(a->scale)) * D(r) * KF(0x3947a718u);
}
PORT_FN(0x00493c70, "power_fn", power_fn_n, fp_none_fn)

// =====================================================================================================================
// the setup set: <user>\setups\<dir>.set (or an old <user><car>.set, imported), 8 setups
// =====================================================================================================================
// FIX CANDIDATE: the path is built in 256 bytes with no bound (a user directory and name longer than ~240 characters
// overrun the frame); the game's names are short.
static void __cdecl load_setup_set_n(const char* car, const char* dir, void* setp) {
    char path[0x100];
    volatile uint8_t ok = 0;
    volatile int32_t fd;
    crt_strcpy(path, ccall<const char*>(F_Win32GetUserDirectory));
    crt_copy(path + crt_strlen(path), (const void*)(uintptr_t)0x004f9624, 8);        // "setups\\"
    crt_strcat(path, dir);
    crt_copy(path + crt_strlen(path), (const void*)(uintptr_t)0x004f962c, 5);        // ".set"
    ok = 0;
    fd = ccall<int32_t>(F_FileOpen, (const char*)path);
    if (fd == 0) {
        const char* user = ccall<const char*>(F_Win32GetUserDirectory);
        UI_sprintf(path, (const char*)0x004f9634, user, car);                        // "%s%s.set"
        fd = ccall<int32_t>(F_FileOpen, (const char*)path);
        if (fd != 0) UI_LogReport((const char*)0x004f9640);                          // "Importing old setups..."
    }
    volatile uint32_t* set = (volatile uint32_t*)setp;
    if (fd != 0) {
        const int32_t size = ccall<int32_t>(F_FileSize, (int32_t)fd);
        if (size == 0x6ac) {
            ccall<uint8_t>(F_FileReadExact, (int32_t)fd, (void*)setp, (int32_t)0x6ac);
            if (set[1] == 0x6acu) {
                const uint32_t ver = set[0];
                if (ver == 1) {
                    ok = 1;
                    for (int32_t i = 0; i < 8; i++) {
                        volatile uint32_t* s = (volatile uint32_t*)((uint8_t*)setp + 0xc + 0xd4 * i);
                        const uint32_t sv = s[0x8c / 4];
                        if (sv != 1 || s[0x90 / 4] != 0xd4u) {
                            UI_LogReport((const char*)0x004f9658, i, sv, 1, (uint32_t)s[0x90 / 4], 0xd4);
                            ccall<void>(F_CarFileLoadDefaultSetup, (void*)s, car, dir);
                        }
                    }
                } else {
                    UI_LogReport((const char*)0x004f968c, (const char*)path, ver, 1);
                }
            } else {
                UI_LogReport((const char*)0x004f96bc, (const char*)path);
            }
        } else {
            UI_LogReport((const char*)0x004f96e0, size);
        }
        ccall<void>(F_FileClose, (int32_t*)&fd);
    } else {
        UI_LogReport((const char*)0x004f96f8, (const char*)path);                    // "Can't open %s\n"
    }
    if (ok == 0) {
        UI_LogReport((const char*)0x004f9708, (const char*)path);                    // "%s: using defaults"
        set[2] = 0;
        set[1] = 0x6ac;
        set[0] = 1;
        for (int32_t i = 0; i < 8; i++) ccall<void>(F_CarFileLoadDefaultSetup, (void*)((uint8_t*)setp + 0xc + 0xd4 * i), car, dir);
    }
}
static void fp_files(Footprint& f, const char*, const char*, void*) { f.replay_only = "reads or writes a file (the setup set)"; }
PORT_FN(0x004921b0, "load_setup_set", load_setup_set_n, fp_files)

// FIX CANDIDATE: as load_setup_set, a 256-byte path with no bound.
static void __cdecl save_setup_set_n(const char* car, const char* dir, void* setp) {
    (void)car;
    char path[0x100];
    volatile int32_t fd;
    crt_strcpy(path, ccall<const char*>(F_Win32GetUserDirectory));
    crt_copy(path + crt_strlen(path), (const void*)(uintptr_t)0x004f971c, 7);        // "setups"
    ccall<uint8_t>(F_FileCreateDirectory, (const char*)path);
    crt_copy(path + crt_strlen(path), (const void*)(uintptr_t)0x004f9724, 2);        // "\\"
    crt_strcat(path, dir);
    crt_copy(path + crt_strlen(path), (const void*)(uintptr_t)0x004f9728, 5);        // ".set"
    fd = ccall<int32_t>(F_FileCreate, (const char*)path);
    if (fd != 0) {
        ccall<uint8_t>(F_FileWrite, (int32_t)fd, (const void*)setp, (int32_t)0x6ac);
        ccall<void>(F_FileClose, (int32_t*)&fd);
        return;
    }
    UI_LogReport((const char*)0x004f9730, (const char*)path);                        // "Can't write setup set %s"
}
PORT_FN(0x00492420, "save_setup_set", save_setup_set_n, fp_files)

// =====================================================================================================================
// the save / load dialogs and the File dialog's buttons
// =====================================================================================================================
static void fp_modal_i(Footprint& f, int32_t) { f.replay_only = "runs a dialog (modal: its own input loop)"; }
static void fp_modal0(Footprint& f) { f.replay_only = "runs a dialog (modal: its own input loop)"; }

// the save dialog: a name (the setup's, without its '*'), a list of the 8 slots; OK stores the setup in the slot picked
// FIX CANDIDATE: a slot outside 0..7 (a damaged .set) stores the setup outside the set.
static uint8_t __cdecl save_setup_dlg_n() {
    struct {
        volatile int32_t sel;                            // F+0x10
        UIDialog dlg;                                    // F+0x14
        UIStringList list;                               // F+0x28
        UIDialogItem items[6];                           // F+0x3c
        char tmp[0x20];                                  // F+0x18c
        // FIX: the name field (item i1c 0x40: 63 characters and the terminator) writes 64 bytes. The original's frame
        // gives it F+0x1ac up to its return address, exactly 64 bytes (48 of them a named buffer, 16 no other local
        // uses), so typing never overran there; the rewrite declares the 64 bytes, so the field stays inside its buffer.
        char name[0x40];                                 // F+0x1ac
    } fr;
    xl_once(M_ONCE_SAVE, 1, 0x0057a770, 0x004f974c, 0x00492a90);
    xl_once(M_ONCE_SAVE, 2, 0x0057aef0, 0x004f9764, 0x00492a80);
    xl_once(M_ONCE_SAVE, 4, 0x0057af20, 0x004f9780, 0x00492a70);
    xl_once(M_ONCE_SAVE, 8, 0x0057b778, 0x004f9788, 0x00492a60);
    const char* title = xl_text(0x0057aef0);
    fr.sel = UI_G32(M_SET_SLOT);
    tcall<void*>(F_UIStringList_ctor, (void*)&fr.list, (int32_t)8, (int32_t)0x20);
    for (uint32_t s = M_SET_NAMES; s < M_SET_END; s += 0xd4) {
        ccall<char*>(F_strncpy, (char*)fr.tmp, (const char*)(uintptr_t)s, (uint32_t)0x1f);
        *(volatile char*)&fr.tmp[0x1f] = 0;
        tcall<void>(F_UIStringList_AddEntry, (void*)&fr.list, (const char*)fr.tmp);
    }
    const char* prompt = xl_text(0x0057a770);
    const char* src = UI_G8(M_SETUP_NAME) == 0x2a ? (const char*)(uintptr_t)(M_SETUP_NAME + 1) : (const char*)(uintptr_t)M_SETUP_NAME;
    // FIX: the setup's name was copied in with no bound: one with no end inside its 64 bytes (the original's '*' shift,
    // MenuEditCar's, could leave a 63-character one ending past the setup, in whatever followed) ran on over the return
    // address. It is held to the field's 64 bytes; a name that ends inside them is the same bytes.
    if (VP_FIX) ui_copy_bounded(fr.name, src, sizeof fr.name);
    else crt_strcpy(fr.name, src);
    it_set(&fr.items[0], 5, 0, 0xa0, 0x20, 0, 0, P(prompt), 0, 0, 0xe, 0, 0, 0, 0);
    it_set(&fr.items[1], 9, 0, 0x28, 0x23, 0xf0, 0x10, 1, 0x40, P(fr.name), 9, 0, 0, 0, 0);
    it_set(&fr.items[2], 0x14, 0, 0x32, 0x42, 0xdc, 0x82, 0, 0, P(&fr.list), 0, 0, 0, P((const void*)&fr.sel), 0);
    it_set(&fr.items[3], 2, (uint32_t)-2, 9, 0xcb, 0, 0, P(xl_text(0x0057af20)), 0, 0, 0, 0, 0, 0, 0);
    it_set(&fr.items[4], 2, (uint32_t)-1, 0xd7, 0xcb, 0, 0, P(xl_text(0x0057b778)), 0, 0, 0, 0, 0, 0, 0);
    it_end(&fr.items[5]);
    dialog(&fr.dlg, title, 0x004f9794, -2, fr.items);                                 // "fdialog.stp"
    const int32_t r = ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, (int32_t)0x140, (int32_t)0xfa, (int32_t)-999,
                                     (int32_t)-999, (int32_t)1);
    if (r == -2) {
        crt_strcpy((char*)(uintptr_t)M_SETUP_NAME, fr.name);
        const int32_t sel = fr.sel;
        UI_G32(M_SET_SLOT) = sel;
        UI_GF(M_SLOT_F) = (float)UI_G32(M_SET_SLOT);
        copy_setup(M_SETUP_ORIG, M_SETUP);
        const int32_t s2 = fr.sel;
        UI_GU32(M_SETUP_SIZE) = 0xd4;
        UI_GU32(M_SETUP_VER) = 1;
        copy_setup(slot_addr(s2), M_SETUP);                                          // FIX CANDIDATE: a slot outside 0..7
        tcall<void>(F_UIStringList_dtor, (void*)&fr.list);
        return 1;
    }
    tcall<void>(F_UIStringList_dtor, (void*)&fr.list);
    return 0;
}
PORT_FN(0x00492560, "save_setup_dlg", save_setup_dlg_n, fp_modal0)

static uint8_t __cdecl save_setup_cb_n(int32_t) {
    ccall<uint8_t>(F_save_setup_dlg);
    return 0;
}
PORT_FN(0x00492550, "save_setup_cb", save_setup_cb_n, fp_modal_i)

// the load dialog: a slot picked from the set's list; OK makes it the current slot
static uint8_t __cdecl load_setup_dlg_n() {
    struct {
        volatile int32_t sel;                            // F+0xc
        UIDialog dlg;                                    // F+0x10
        UIStringList list;                               // F+0x24
        UIDialogItem items[5];                           // F+0x38
        char tmp[0x20];                                  // F+0x150
    } fr;
    fr.sel = UI_G32(M_SET_SLOT);
    tcall<void*>(F_UIStringList_ctor, (void*)&fr.list, (int32_t)8, (int32_t)0x20);
    for (uint32_t s = M_SET_NAMES; s < M_SET_END; s += 0xd4) {
        ccall<char*>(F_strncpy, (char*)fr.tmp, (const char*)(uintptr_t)s, (uint32_t)0x1f);
        *(volatile char*)&fr.tmp[0x1f] = 0;
        tcall<void>(F_UIStringList_AddEntry, (void*)&fr.list, (const char*)fr.tmp);
    }
    xl_once(M_ONCE_LOAD, 1, 0x0057af00, 0x004f97f4, 0x00493010);
    xl_once(M_ONCE_LOAD, 2, 0x0057b650, 0x004f980c, 0x00493000);
    xl_once(M_ONCE_LOAD, 4, 0x0057a728, 0x004f9814, 0x00492ff0);
    xl_once(M_ONCE_LOAD, 8, 0x0057b2f8, 0x004f9820, 0x00492fe0);
    const char* title = xl_text(0x0057b2f8);
    it_set(&fr.items[0], 5, 0, 0xa0, 0x20, 0, 0, P(xl_text(0x0057af00)), 0, 0, 0xe, 0, 0, 0, 0);
    it_set(&fr.items[1], 0x14, 0, 0x32, 0x42, 0xdc, 0x82, 0, 0, P(&fr.list), 0, 0, 0, P((const void*)&fr.sel), 0);
    it_set(&fr.items[2], 2, (uint32_t)-2, 9, 0xcb, 0, 0, P(xl_text(0x0057b650)), 0, 0, 0, 0, 0, 0, 0);
    it_set(&fr.items[3], 2, (uint32_t)-1, 0xd7, 0xcb, 0, 0, P(xl_text(0x0057a728)), 0, 0, 0, 0, 0, 0, 0);
    it_end(&fr.items[4]);
    dialog(&fr.dlg, title, 0x004f983c, -2, fr.items);                                 // "fdialog.stp"
    const int32_t r = ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, (int32_t)0x140, (int32_t)0xfa, (int32_t)-999,
                                     (int32_t)-999, (int32_t)1);
    if (r == -2) {
        UI_G32(M_SET_SLOT) = fr.sel;
        UI_GF(M_SLOT_F) = (float)UI_G32(M_SET_SLOT);
        tcall<void>(F_UIStringList_dtor, (void*)&fr.list);
        return 1;
    }
    tcall<void>(F_UIStringList_dtor, (void*)&fr.list);
    return 0;
}
PORT_FN(0x00492c10, "load_setup_dlg", load_setup_dlg_n, fp_modal0)

// Load: an unsaved change offered for saving first (Yes saves -- into its slot if it has a name, else by the save
// dialog --, No drops it, Cancel stops); then the load dialog, whose slot is copied into the setup
static uint8_t __cdecl load_setup_cb_n(int32_t) {
    xl_once(M_ONCE_LOAD_CB, 1, 0x0057b2e8, 0x004f97a0, 0x00493030);
    xl_once(M_ONCE_LOAD_CB, 2, 0x0057af10, 0x004f97c4, 0x00493020);
    if (setup_changed()) {
        const char* text = xl_text(0x0057af10);
        const char* title = xl_text(0x0057b2e8);
        const int32_t r = ccall<int32_t>(F_UIDoYesNoCancelBox, title, text);
        if (r == -2) {
            if (UI_G8(M_SETUP_NAME) == 0) {
                if (!ccall<uint8_t>(F_save_setup_dlg)) return 0;
            } else {
                copy_setup(M_SETUP_ORIG, M_SETUP);
                const int32_t slot = UI_G32(M_SET_SLOT);
                UI_GU32(M_SETUP_SIZE) = 0xd4;
                UI_GU32(M_SETUP_VER) = 1;
                copy_setup(slot_addr(slot), M_SETUP);
            }
        } else if (r == -1) {
            return 0;
        }
    }
    if (ccall<uint8_t>(F_load_setup_dlg)) {
        copy_setup(M_SETUP, slot_addr(UI_G32(M_SET_SLOT)));                          // FIX CANDIDATE: a slot outside 0..7
        copy_setup(M_SETUP_ORIG, M_SETUP);
    }
    return 0;
}
PORT_FN(0x00492aa0, "load_setup_cb", load_setup_cb_n, fp_modal_i)

// Default: an unsaved change offered for saving first; then the car's default setup, unnamed
static uint8_t __cdecl default_setup_cb_n(int32_t) {
    xl_once(M_ONCE_DEFAULT, 1, 0x0057a7a8, 0x004f9848, 0x00493150);
    xl_once(M_ONCE_DEFAULT, 2, 0x0057a7b8, 0x004f986c, 0x00493140);
    if (setup_changed()) {
        const char* text = xl_text(0x0057a7b8);
        const char* title = xl_text(0x0057a7a8);
        const int32_t r = ccall<int32_t>(F_UIDoYesNoCancelBox, title, text);
        if (r == -2) {
            if (!ccall<uint8_t>(F_save_setup_dlg)) return 0;
        } else if (r == -1) {
            return 0;
        }
    }
    ccall<void>(F_CarFileLoadDefaultSetup, (void*)(uintptr_t)M_SETUP, UI_GP(const char, M_CAR_NAME), UI_GP(const char, M_CAR_DIR));
    UI_G8(M_SETUP_NAME) = 0;
    return 0;
}
PORT_FN(0x00493040, "default_setup_cb", default_setup_cb_n, fp_modal_i)

// the File button: a dialog of one FileControl (the setup's summary and its Load / Save / Default buttons) and Back
static uint8_t __cdecl file_cb_n(int32_t) {
    struct {
        UIDialog dlg;                                    // F+8
        FileControl fc;                                  // F+0x1c
        UIDialogItem items[4];                           // F+0x38
    } fr;
    xl_once(M_ONCE_FILE_CB, 1, 0x0057aed0, 0x004f95fc, 0x00492100);
    xl_once(M_ONCE_FILE_CB, 2, 0x0057af40, 0x004f960c, 0x004920f0);
    fr.fc.widget = 0;
    fr.fc.vtbl = (const void*)(uintptr_t)VT_FileControl;
    fr.fc.stamp = ccall<void*>(F_gxGetStamp, (const char*)0x004f9cb8);               // "gsidecar.stp"
    const char* title = UI_GP(const char, M_TITLE);
    if (title == 0) title = (const char*)0x004f9614;
    it_set(&fr.items[0], 5, 0, 0x190, 0x16, 0, 0, P(title), 0, 0, 0x10, 0, 0, 0, 0);
    it_set(&fr.items[1], 0x17, 0, 0x28, 0x82, 0x208, 0xd2, T_EMPTY, 0, P(&fr.fc), 0, 0, 0, 0, 0);
    it_set(&fr.items[2], 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, P(xl_text(0x0057af40)), 0, 0, 6, 0, 0, 0, 0);
    it_end(&fr.items[3]);
    dialog(&fr.dlg, xl_text(0x0057aed0), 0x004f9618, -2, fr.items);                  // "main_t.stp"
    ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H), (int32_t)-999, (int32_t)-999,
                   (int32_t)1);
    fr.fc.vtbl = (const void*)(uintptr_t)VT_FileControl;
    ccall<void>(F_gxForgetStamp, (void*)fr.fc.stamp);
    return 0;
}
PORT_FN(0x00491e70, "file_cb", file_cb_n, fp_modal_i)

// =====================================================================================================================
// export_setup: the setup as an HTML table, <user>setups\<dir>.html
// =====================================================================================================================
typedef void(__cdecl* HtmlLn_t)(const char*, ...);
typedef void(__cdecl* HtmlW_t)(const void*, const char*, ...);
#define HTML_LN ((HtmlLn_t)(uintptr_t)F_HTMLWriteLn)
static const char* const ROW2 = (const char*)0x004f9ac0;    // "<TR><TD>%s</TD><TD ALIGN=CENTER>%0.*f</TD><TD ...>%0.*f</TD></TR>"
// a setting that sits between the car file's two limits: ((hi - lo) * setting) + lo
static __forceinline double lerp(uint32_t hi, uint32_t lo, const volatile float* s) { return ((D(MF(hi)) - D(MF(lo))) * D(*s)) + D(MF(lo)); }
// FIX CANDIDATE: the path is sprintf'd into 256 bytes with no bound (a long user directory or name overruns the frame).
static void __cdecl export_setup_n(const char* car, const char* dir, const void* setup) {
    (void)car;
    char path[0x100];
    xl_once(M_ONCE_EXPORT1, 0x01, 0x0057b538, 0x004f989c, 0x00493c30);
    xl_once(M_ONCE_EXPORT1, 0x02, 0x0057b2a8, 0x004f98ac, 0x00493c20);
    xl_once(M_ONCE_EXPORT1, 0x04, 0x0057b308, 0x004f98b8, 0x00493c10);
    xl_once(M_ONCE_EXPORT1, 0x08, 0x0057b708, 0x004f98cc, 0x00493c00);
    xl_once(M_ONCE_EXPORT1, 0x10, 0x0057b338, 0x004f98e4, 0x00493bf0);
    xl_once(M_ONCE_EXPORT1, 0x20, 0x0057b660, 0x004f98fc, 0x00493be0);
    xl_once(M_ONCE_EXPORT1, 0x40, 0x0057b328, 0x004f9914, 0x00493bd0);
    xl_once(M_ONCE_EXPORT1, 0x80, 0x0057aeb0, 0x004f9928, 0x00493bc0);
    xl_once(M_ONCE_EXPORT2, 0x01, 0x0057b2c8, 0x004f9940, 0x00493bb0);
    xl_once(M_ONCE_EXPORT2, 0x02, 0x0057b738, 0x004f9958, 0x00493ba0);
    xl_once(M_ONCE_EXPORT2, 0x04, 0x0057af30, 0x004f9970, 0x00493b90);
    xl_once(M_ONCE_EXPORT2, 0x08, 0x0057aee0, 0x004f9988, 0x00493b80);
    xl_once(M_ONCE_EXPORT2, 0x10, 0x0057a740, 0x004f99a8, 0x00493b70);
    xl_once(M_ONCE_EXPORT2, 0x20, 0x0057b640, 0x004f99c4, 0x00493b60);
    xl_once(M_ONCE_EXPORT2, 0x40, 0x0057b6c0, 0x004f99dc, 0x00493b50);
    xl_once(M_ONCE_EXPORT2, 0x80, 0x0057b2d8, 0x004f99f0, 0x00493b40);
    const char* user = ccall<const char*>(F_Win32GetUserDirectory);
    UI_sprintf(path, (const char*)0x004f9a00, user, dir);                             // "%ssetups\\%s.html"
    if (!ccall<uint8_t>(F_HTMLBegin, (const char*)path, xl_text(0x0057b2d8))) {
        UI_LogReport((const char*)0x004f9c80, (const char*)path);
        return;
    }
    ((HtmlW_t)(uintptr_t)F_HTMLWrite)((const void*)0x004dec68, (const char*)0x004f9a14, xl_text(0x0057b2d8));
    const volatile float* s = (const volatile float*)setup;
    const char* t = xl_text(0x0057b6c0);
    HTML_LN((const char*)0x004f9a18, t, (const char*)setup + 0x94);                   // "<B>%s:</B> %s"
    HTML_LN((const char*)0x004f9a28);
    HTML_LN((const char*)0x004f9a44);
    HTML_LN((const char*)0x004f9a4c);
    // the springs, shocks and anti-roll bars as percentages (front, rear)
    static const uint32_t pct[4][3] = {{0x0057b308, 0x10, 0x18}, {0x0057b708, 0x20, 0x28}, {0x0057b338, 0x38, 0x3c}, {0x0057b660, 0x30, 0x34}};
    for (int i = 0; i < 4; i++) {
        t = xl_text(pct[i][0]);
        const double b = D(s[pct[i][2] / 4]) * KF(K_100);
        const double a = D(s[pct[i][1] / 4]) * KF(K_100);
        HTML_LN(ROW2, t, 0, a, 0, b);
    }
    // the alignment in the car file's units, one decimal
    t = xl_text(0x0057b328);
    {
        const double b = lerp(0x0057b1a0, 0x0057b170, &s[0x84 / 4]);
        const double a = lerp(0x0057b19c, 0x0057b16c, &s[0x80 / 4]);
        HTML_LN(ROW2, t, 1, a, 1, b);
    }
    t = xl_text(0x0057aeb0);
    {
        const double b = lerp(0x0057b198, 0x0057b168, &s[0x7c / 4]);
        const double a = lerp(0x0057b194, 0x0057b164, &s[0x78 / 4]);
        HTML_LN(ROW2, t, 1, a, 1, b);
    }
    t = xl_text(0x0057b2c8);
    {
        const double b = lerp(0x0057b064, 0x0057b060, &s[0x44 / 4]);
        const double a = lerp(0x0057b05c, 0x0057b058, &s[0x40 / 4]);
        HTML_LN(ROW2, t, 1, a, 1, b);
    }
    HTML_LN((const char*)0x004f9b0c);
    HTML_LN((const char*)0x004f9b1c);
    HTML_LN((const char*)0x004f9b30);
    HTML_LN((const char*)0x004f9b40);
    {
        const char* t2 = xl_text(0x0057b2a8);
        const char* t1 = xl_text(0x0057b538);
        HTML_LN((const char*)0x004f9b48, t1, t2);
    }
    t = xl_text(0x0057b640);
    {
        const double b = D(s[0x4c / 4]) * KF(K_100);
        const double a = D(s[0x48 / 4]) * KF(K_100);
        HTML_LN(ROW2, t, 0, a, 0, b);
    }
    HTML_LN((const char*)0x004f9bb8);
    t = xl_text(0x0057a740);
    HTML_LN(ROW2, t, 2, D(s[0x50 / 4]), 2, D(s[0x5c / 4]));
    HTML_LN(ROW2, (const char*)0x004f9bc4, 2, D(s[0x54 / 4]), 2, D(s[0x60 / 4]));
    HTML_LN(ROW2, (const char*)0x004f9bc8, 2, D(s[0x58 / 4]), 2, D(s[0x64 / 4]));
    t = xl_text(0x0057aee0);
    HTML_LN((const char*)0x004f9bcc, t, lerp(0x0057b104, 0x0057b100, &s[0]));
    t = xl_text(0x0057af30);
    HTML_LN((const char*)0x004f9bfc, t, D(s[0x88 / 4]) * D(MF(0x0057b1d4)));
    t = xl_text(0x0057b738);
    {
        const int32_t bias = *(const volatile int32_t*)&s[2];
        const int32_t c = bias >= 0x3f000000 ? 0x52 : 0x46;                           // 'R' / 'F' (movsx)
        HTML_LN((const char*)0x004f9c2c, t, (D(s[2]) * KF(K_100)) - KF(0x42480000u), c);
    }
    HTML_LN((const char*)0x004f9c60);
    HTML_LN((const char*)0x004f9c6c);
    ccall<void>(F_HTMLEnd);
}
static void fp_export(Footprint& f, const char*, const char*, const void*) { f.replay_only = "writes a file (the setup as HTML)"; }
PORT_FN(0x004932a0, "export_setup", export_setup_n, fp_export)

// =====================================================================================================================
// MenuEditCar: the garage
// =====================================================================================================================
// car: the car's name ("<car>.car" its resources, "<car>.cf" its car file); dir: the setup's (and the set's) name;
// flags: CarFileLoad's; title: the dialog's (0: none). 1 if the dialog ended with the Race button (-2).
// (garage_cb's car is a 64-byte static, 0x505d38; the multiplayer chooser's its car viewer's 32-byte name.)
static uint8_t __cdecl MenuEditCar_n(const char* car, const char* dir, uint8_t* flags, const char* title) {
    // FIX: "<car>.car" was sprintf'd into 32 bytes with the item list after it, so a car name over 27 characters was
    // overwritten there by the items before ResourceSetUnload read it back (the wrong resource set unloaded), and
    // "<car>.cf" copied into 240 bytes at the frame's top (a name over 236 characters ran on over the return address).
    // The rewrite's "<car>.car" buffer is 240 bytes like "<car>.cf"'s, and a name too long for them (no caller
    // passes one, above) is cut to its first 235 characters in both. A name that fits is the same bytes.
    enum : uint32_t { NAME_MAX = 0xeb };
    struct {
        volatile uint8_t ret;                            // F+0x13
        AlignControl align;                              // F+0x14
        UIDialog dlg;                                    // F+0x48
        BalanceControl balance;                          // F+0x5c
        ChassisControl chassis;                          // F+0x7c
        DrivetrainControl drive;                         // F+0xa4 (its TorqueCurveControl at F+0xc4)
        AeroControl aero;                                // F+0xe8
        TorqueCurveControl spare;                        // F+0x110: never added (only its stamp is ever stored)
        char res[VP_FIX ? 0xf0 : 0x20];                  // F+0x12c "<car>.car" (FIX: above)
        UIDialogItem items[22];                          // F+0x14c
        char cf[0xf0];                                   // F+0x61c "<car>.cf"
    } fr;
    const bool cut = VP_FIX && ui_strnlen(car, NAME_MAX) > NAME_MAX;                 // FIX: (above)
    if (cut) {
        ui_copy_bounded(fr.res, car, NAME_MAX + 1);
        crt_copy(fr.res + NAME_MAX, (const void*)(uintptr_t)0x004f94d2, 5);           // ".car" (the format's tail)
    } else {
        UI_sprintf(fr.res, (const char*)0x004f94d0, car);                            // "%s.car"
    }
    ccall<void>(F_ResourceSetMustLoad, (const char*)fr.res);
    UI_GP(const char, M_CAR_NAME) = car;
    UI_GP(const char, M_CAR_DIR) = dir;
    UI_GP(const char, M_TITLE) = title;
    if (!ccall<uint8_t>(F_CarFileLoadSetup, (void*)(uintptr_t)M_SETUP, car, dir))
        UI_LogReport((const char*)0x004f94d8, UI_GP(const char, M_CAR_NAME));        // "Couldn't load setup %s"
    if (UI_G8(M_SETUP_NAME) == 0x2a) {
        // the name without its '*' (an unsaved setup), and a loaded copy that can't match: it's saved on the way out
        if (UI_G8(M_SETUP_NAME) != 0) {
            volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)(M_SETUP_NAME + 1);
            do {
                p[-1] = p[0];
                p++;
            } while (p[-1] != 0);
        }
        volatile uint32_t* o = (volatile uint32_t*)(uintptr_t)M_SETUP_ORIG;
        for (int i = 0; i < 0x35; i++) o[i] = 0xa3a3a3a3u;
    } else {
        copy_setup(M_SETUP_ORIG, M_SETUP);
    }
    ccall<void>(F_load_setup_set, car, dir, (void*)(uintptr_t)M_SET);
    UI_GF(M_SLOT_F) = (float)UI_G32(M_SET_SLOT);
    if (cut) ui_copy_bounded(fr.cf, car, NAME_MAX + 1);                              // FIX: (above)
    else crt_strcpy(fr.cf, car);
    crt_copy(fr.cf + crt_strlen(fr.cf), (const void*)(uintptr_t)0x004f94f0, 4);     // ".cf"
    if (!ccall<uint8_t>(F_CarFileLoad_file, (void*)(uintptr_t)M_CARFILE, (const char*)fr.cf, flags)) {
        UI_LogReport((const char*)0x004f94f4, (const char*)fr.cf);                   // "Couldn't load carfile: %s"
        ccall<void>(F_ResourceSetUnload, (const char*)fr.res);
        return 0;
    }
    ccall<uint8_t>(F_CarFileLoad_data, (void*)(uintptr_t)M_CARDATA, (const void*)(uintptr_t)M_SETUP, (const char*)fr.cf, flags);
    // the controls (their constructors inlined)
    fr.chassis.widget = 0;
    UI_GU32(M_57A7C4) = UI_GU32(M_CARFILE_108);
    fr.chassis.vtbl = (const void*)(uintptr_t)VT_ChassisControl;
    fr.chassis.stamp[0] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d24);       // "ggraph.stp"
    fr.chassis.stamp[1] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d18);       // "gshocks.stp"
    fr.chassis.stamp[2] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d08);       // "gsprings.stp"
    fr.align.widget = 0;
    fr.chassis.page = 1;
    fr.align.vtbl = (const void*)(uintptr_t)VT_AlignControl;
    fr.align.stamp[0] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d24);
    fr.align.stamp[1] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9cf8);         // "gcamicon.stp"
    fr.align.stamp[2] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9ce8);         // "gcamicor.stp"
    fr.align.stamp[3] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9cd8);         // "gtoeicon.stp"
    fr.align.stamp[4] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9cc8);         // "gtoeicor.stp"
    fr.align.stamp[5] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9cb8);         // "gsidecar.stp"
    fr.drive.widget = 0;
    fr.drive.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    fr.drive.torque.vtbl = (const void*)(uintptr_t)VT_TorqueCurveControl;
    fr.drive.torque.widget = 0;
    fr.drive.torque.stamp = ccall<void*>(F_gxGetStamp, (const char*)0x004f9cac);     // "gtorque.stp"
    fr.drive.vtbl = (const void*)(uintptr_t)VT_DrivetrainControl;
    ccall<void>(F_TireBegin);
    fr.drive.stamp[0] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d24);
    fr.drive.stamp[1] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9ca0);         // "ggears.stp"
    fr.drive.page = 6;
    fr.spare.stamp = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d24);
    fr.aero.widget = 0;
    fr.aero.vtbl = (const void*)(uintptr_t)VT_AeroControl;
    fr.aero.stamp[0] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d24);
    fr.aero.stamp[1] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9c90);          // "gaericon.stp"
    UI_G32(M_ONE) = 1;
    xl_once(M_ONCE1, 0x01, 0x0057b268, 0x004f9510, 0x004921a0);
    xl_once(M_ONCE1, 0x02, 0x0057b728, 0x004f9520, 0x00492190);
    xl_once(M_ONCE1, 0x04, 0x0057b318, 0x004f9534, 0x00492180);
    xl_once(M_ONCE1, 0x08, 0x0057b690, 0x004f9548, 0x00492170);
    xl_once(M_ONCE1, 0x10, 0x0057b298, 0x004f9560, 0x00492160);
    xl_once(M_ONCE1, 0x20, 0x0057a750, 0x004f9574, 0x00492150);
    xl_once(M_ONCE1, 0x40, 0x0057b548, 0x004f9588, 0x00492140);
    xl_once(M_ONCE1, 0x80, 0x0057aec0, 0x004f959c, 0x00492130);
    xl_once(M_ONCE2, 0x01, 0x0057ae90, 0x004f95b0, 0x00492120);
    xl_once(M_ONCE2, 0x02, 0x0057af50, 0x004f95c4, 0x00492110);
    fr.balance.widget = 0;
    fr.balance.vtbl = (const void*)(uintptr_t)VT_BalanceControl;
    fr.balance.stamp[0] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d40);       // "gbalance.stp"
    fr.balance.stamp[1] = ccall<void*>(F_gxGetStamp, (const char*)0x004f9d30);       // "gbalicon.stp"
    if (title == 0) title = (const char*)0x004f95cc;
    // the dialog: the title, the balance panel, four pages (chassis, alignment, drivetrain, aero) each a group shown by
    // its radio button, Back, Race and File
    UIDialogItem* it = fr.items;
    it_ctor(&it[0], 5, 0, 0x190, 0x16, 0, 0, P(title), 0, 0, 0x10, 0, 0, 0, 0);
    it_ctor(&it[1], 0x17, 0, 0x19a, 0x143, 0xbf, 0x36, T_EMPTY, 0, P(&fr.balance), 0, 0, 0, 0, 0);
    it_ctor(&it[2], 1, 0, 0, 0, 0, 0, 0, 0, 0x0057a76c, 0, 0, 0, 0, 0);
    it_ctor(&it[3], 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, T_EMPTY, 0, P(&fr.chassis), 0, 0, 0, 0, 0);
    it_ctor(&it[4], 1, 1, 0, 0, 0, 0, 0, 0, 0x0057a76c, 0, 0, 0, 0, 0);
    it_ctor(&it[5], 1, 0, 0, 0, 0, 0, 0, 0, 0x0057a794, 0, 0, 0, 0, 0);
    it_ctor(&it[6], 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, T_EMPTY, 0, P(&fr.align), 0, 0, 0, 0, 0);
    it_ctor(&it[7], 1, 1, 0, 0, 0, 0, 0, 0, 0x0057a794, 0, 0, 0, 0, 0);
    it_ctor(&it[8], 1, 0, 0, 0, 0, 0, 0, 0, 0x0057a738, 0, 0, 0, 0, 0);
    it_ctor(&it[9], 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, T_EMPTY, 0, P(&fr.drive), 0, 0, 0, 0, 0);
    it_ctor(&it[10], 1, 1, 0, 0, 0, 0, 0, 0, 0x0057a738, 0, 0, 0, 0, 0);
    it_ctor(&it[11], 1, 0, 0, 0, 0, 0, 0, 0, 0x0057b6ec, 0, 0, 0, 0, 0);
    it_ctor(&it[12], 0x17, 0, 0x19a, 0x81, 0xbf, 0xbd, T_EMPTY, 0, P(&fr.aero), 0, 0, 0, 0, 0);
    it_ctor(&it[13], 1, 1, 0, 0, 0, 0, 0, 0, 0x0057b6ec, 0, 0, 0, 0, 0);
    it_ctor(&it[14], 0xc, 0, 0x21, 0x58, (uint32_t)-1, 0, P(xl_text(0x0057ae90)), 0x0057a76c, M_ONE, 3, 0, 0, 0, 0);
    it_ctor(&it[15], 0xc, 0, 0xad, 0x58, (uint32_t)-1, 0, P(xl_text(0x0057aec0)), 0x0057a794, M_ONE, 3, 0, 0, 0, 0);
    it_ctor(&it[16], 0xc, 0, 0x139, 0x58, (uint32_t)-1, 0, P(xl_text(0x0057b690)), 0x0057a738, M_ONE, 3, 0, 0, 0, 0);
    it_set(&it[17], 0xc, 0, 0x1c5, 0x58, (uint32_t)-1, 0, P(xl_text(0x0057b298)), 0x0057b6ec, M_ONE, 3, 0, 0, 0, 0);
    it_ctor(&it[18], 2, (uint32_t)-1, 0x13, 0x1a2, 0, 0, P(xl_text(0x0057af50)), 0, 0, 6, 0, 0, 0, 0);
    it_ctor(&it[19], 2, (uint32_t)-2, 0x1e5, 0x1a5, 0, 0, P(xl_text(0x0057b548)), 0, 0, 2, 0, 0, 0, 0);
    it_ctor(&it[20], 2, 0, 0x154, 0x1a5, 0, 0, P(xl_text(0x0057a750)), 0, F_file_cb, 2, 0, 0, 0, 0);
    it_end(&it[21]);
    dialog(&fr.dlg, xl_text(0x0057b268), 0x004f95d0, -2, fr.items);                  // "main_t.stp"
    const int32_t r = ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, UI_G32(S_SCREEN_W), UI_G32(S_SCREEN_H),
                                     (int32_t)-999, (int32_t)-999, (int32_t)1);
    fr.ret = (uint8_t)(r == -2 ? 1 : 0);
    ccall<void>(F_save_setup_set, car, dir, (void*)(uintptr_t)M_SET);
    ccall<void>(F_export_setup, car, dir, (const void*)(uintptr_t)M_SETUP);
    if (setup_changed()) {
        // unsaved: '*' before the name (the name and the byte after its end each moved one place on)
        int32_t n = (int32_t)crt_strlen((const char*)(uintptr_t)M_SETUP_NAME) + 2;
        // FIX: a name of 62 characters or more (the save dialog takes 63) moved its end past the setup's 64-byte name:
        // 62 put the byte after the terminator past the setup, 63 the terminator too, so the setup was saved with a
        // name that doesn't end. The name is kept inside its 64 bytes: '*', its first 62 characters, the terminator
        // (a 62-character name comes out the same; a 63-character one loses its last character).
        if (VP_FIX && n > 0x3f) {
            for (uint32_t k = 0x3e; k != 0; k--) UI_G8(M_SETUP_NAME + k) = UI_G8(M_SETUP_NAME + k - 1);
            UI_G8(M_SETUP_NAME + 0x3f) = 0;
        } else if (n > 0) {
            do {
                UI_G8(M_SETUP_NAME + (uint32_t)n) = UI_G8(M_SETUP_NAME - 1 + (uint32_t)n);
            } while (--n != 0);
        }
        UI_G8(M_SETUP_NAME) = 0x2a;
    }
    if (!ccall<uint8_t>(F_CarFileSaveSetup, (const void*)(uintptr_t)M_SETUP, car, dir))
        UI_LogReport((const char*)0x004f95dc, car);                                  // "Couldn't save car setup (%s)"
    ccall<void>(F_ResourceSetUnload, (const char*)fr.res);
    // the controls' destructors (inlined), in reverse
    fr.balance.vtbl = (const void*)(uintptr_t)VT_BalanceControl;
    ccall<void>(F_gxForgetStamp, (void*)fr.balance.stamp[0]);
    ccall<void>(F_gxForgetStamp, (void*)fr.balance.stamp[1]);
    fr.balance.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    fr.aero.vtbl = (const void*)(uintptr_t)VT_AeroControl;
    ccall<void>(F_gxForgetStamp, (void*)fr.aero.stamp[0]);
    ccall<void>(F_gxForgetStamp, (void*)fr.aero.stamp[1]);
    fr.aero.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    ccall<void>(F_gxForgetStamp, (void*)fr.spare.stamp);
    fr.drive.vtbl = (const void*)(uintptr_t)VT_DrivetrainControl;
    ccall<void>(F_gxForgetStamp, (void*)fr.drive.stamp[0]);
    ccall<void>(F_gxForgetStamp, (void*)fr.drive.stamp[1]);
    ccall<void>(F_TireEnd);
    fr.drive.torque.vtbl = (const void*)(uintptr_t)VT_TorqueCurveControl;
    ccall<void>(F_gxForgetStamp, (void*)fr.drive.torque.stamp);
    fr.drive.torque.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    fr.align.vtbl = (const void*)(uintptr_t)VT_AlignControl;
    fr.drive.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    for (int i = 0; i < 6; i++) ccall<void>(F_gxForgetStamp, (void*)fr.align.stamp[i]);
    fr.align.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    fr.chassis.vtbl = (const void*)(uintptr_t)VT_ChassisControl;
    for (int i = 0; i < 3; i++) ccall<void>(F_gxForgetStamp, (void*)fr.chassis.stamp[i]);
    return fr.ret;
}
static void fp_MenuEditCar(Footprint& f, const char*, const char*, uint8_t*, const char*) {
    f.replay_only = "the garage: loads the car and its setups, runs its dialog (modal), saves and exports the setup";
}
PORT_FN(0x004911a0, "MenuEditCar", MenuEditCar_n, fp_MenuEditCar)

// =====================================================================================================================
// the controls: shared pieces
// =====================================================================================================================
static __forceinline void notify(void* self, int32_t id, uint32_t p, uint32_t size) {
    tcall<void>(F_UICC_AddNotification, self, id, (const void*)(uintptr_t)p, size);
}
static __forceinline void dirty(void* self) { tcall<void>(F_UICC_Dirty, self); }
static __forceinline void add_items(void* self, UIDialogItem* items) { tcall<void>(F_UICC_AddItems, self, items); }
static __forceinline void stamp_at(void* s, int32_t x, int32_t y, int32_t frame) {
    ccall<void>(F_gxDrawStamp, s, x, y, frame, (const void*)0);
}
static __forceinline void style_draw(int32_t style, int32_t x, int32_t y, const char* text) {
    ccall<void>(F_UIStyleDraw, style, x, y, text, (uint32_t)0);
}
static __forceinline void rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) { ccall<void>(F_gxRect, x0, y0, x1, y1, c); }
static __forceinline void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) { ccall<void>(F_gxLine, x0, y0, x1, y1, c); }
static __forceinline void locale(char* s) { ccall<void>(F_LocaleConvertNumeric, s); }
// GraphInfo (graph.obj): the plot's rectangle, a tick spacing, the axes' ranges
struct GraphInfo { volatile int32_t x0, y0, x1, y1, tick; volatile uint32_t xlo, xhi, ylo, yhi; };
static __forceinline void graph(GraphInfo* g, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t tick, uint32_t xlo,
                                uint32_t xhi, uint32_t ylo, uint32_t yhi) {
    g->x0 = x0; g->y0 = y0; g->x1 = x1; g->y1 = y1; g->tick = tick; g->xlo = xlo; g->xhi = xhi; g->ylo = ylo; g->yhi = yhi;
}
static __forceinline void graph_axes(const GraphInfo* g, int32_t a, int32_t b, uint32_t c) { ccall<void>(F_GraphDrawAxes, g, a, b, c); }
static __forceinline void graph_plot(const GraphInfo* g, uint32_t fn, const void* arg, uint32_t c) {
    ccall<void>(F_GraphDrawLinePlot, g, (const void*)(uintptr_t)fn, arg, c);
}

// footprints
static __forceinline bool once_all(uint32_t guard, uint8_t bits) { return (UI_G8(guard) & bits) == bits; }
static __forceinline void fp_xl(Footprint& f, uint32_t xl) { f.add((void*)(uintptr_t)xl, 0xc, "an Xlator"); }
// a draw: the canvas it's given (made current), the current-canvas static; replay_only while its Xlators aren't built
static bool fp_draw_base(Footprint& f, gxCanvas* c, uint32_t guard, uint8_t bits) {
    if (guard && !once_all(guard, bits)) { f.replay_only = "builds its translated strings (function-local Xlators, atexit) the first time"; return false; }
    UI_FP_CANVAS(f, c);
    UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");
    return true;
}
template <typename T> static void fp_create(Footprint& f, T*, Edx) { f.replay_only = "adds notifications (allocates their copies)"; }
template <typename T> static void fp_added(Footprint& f, T*, Edx) { f.replay_only = "adds its widgets to the dialog (allocates, loads stamps)"; }
template <typename T> static void fp_dtor(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "forgets its stamps, frees the control"; }
// Callback: the control (its page) and its CustomWidget (Dirty)
template <typename T> static void fp_callback(Footprint& f, T* self, Edx, int32_t, const void*) {
    f.add((void*)self, sizeof(T), "the control");
    if (self->widget) f.add((void*)self->widget, sizeof(CustomWidget), "its CustomWidget");
}

// the bar clamp the balance uses: (a - b) / max(a + b, 0.01), within -1..1 (fld c; fcom st(1); ...; fstp st(0))
static __forceinline double balance_ratio(double num, double den) {
    den = !(KF(0x3c23d70au) >= den) ? den : KF(0x3c23d70au);
    double r = num / den;
    r = KF(K_1) > r ? r : KF(K_1);
    r = !(KF(0xbf800000u) >= r) ? r : KF(0xbf800000u);
    return r;
}

// =====================================================================================================================
// BalanceControl: four bars -- total, aero, alignment, shocks -- from -1 (front) to 1 (rear)
// =====================================================================================================================
static void __fastcall BalanceControl_Create_n(BalanceControl* self, Edx) { notify(self, 0, M_SETUP, 0x8c); }
PORT_FN(0x00493d50, "BalanceControl::Create", BalanceControl_Create_n, fp_create<BalanceControl>)

static void __fastcall BalanceControl_Callback_n(BalanceControl* self, Edx, int32_t, const void*) { dirty(self); }
PORT_FN(0x00493d70, "BalanceControl::Callback", BalanceControl_Callback_n, fp_callback<BalanceControl>)

// a bar: from the centre, rightwards in one colour for a positive value, leftwards in another
static __forceinline void balance_bar(BalanceControl* self, int32_t ebx, int32_t edi, double len, double v, uint32_t vbits) {
    (void)v;
    const int32_t x = self->x;
    if ((int32_t)vbits > 0) {
        const uint32_t c = UI_GU32(M_C_57A734);
        const int32_t t = x87_ftol(len);
        rect(add32(x, edi), ebx, add32(add32(t, x), edi), ebx + 5, c);
    } else {
        const uint32_t c = UI_GU32(M_C_57A7C8);
        const int32_t t = x87_ftol(len);
        rect(add32(add32(x, t), edi), ebx, add32(x, edi), ebx + 5, c);
    }
}
static void __fastcall BalanceControl_Draw_n(BalanceControl* self, Edx, gxCanvas* canvas) {
    xl_once(M_ONCE_BALANCE, 1, 0x005d5b70, 0x004f9da0, 0x00494330);
    xl_once(M_ONCE_BALANCE, 2, 0x005d5b80, 0x004f9d84, 0x00494320);
    xl_once(M_ONCE_BALANCE, 4, 0x005d5b90, 0x004f9d6c, 0x00494310);
    xl_once(M_ONCE_BALANCE, 8, 0x005d5ba0, 0x004f9d50, 0x00494300);
    ccall<void>(F_CarFileCombine, (void*)(uintptr_t)M_CARDATA, (const void*)(uintptr_t)M_SETUP, (const void*)(uintptr_t)M_CARFILE);
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp[0], self->x, self->y, 0);
    stamp_at(self->stamp[1], self->x + 0x41, self->y + 7, 0);
    volatile float l10, l14, l18, l1c, l20;
    // the springs (front 0x57af70 + 0x57af80 + 0x57af90 + 0x57af98, rear the odd ones)
    l14 = (float)(((D(MF(0x0057af90)) + D(MF(0x0057af98))) + D(MF(0x0057af80))) + D(MF(0x0057af70)));
    const double b = ((D(MF(0x0057af94)) + D(MF(0x0057af9c))) + D(MF(0x0057af88))) + D(MF(0x0057af78));
    l10 = (float)b;
    l18 = (float)balance_ratio(D(l14) - b, D(l10) + D(l14));
    // the anti-roll bars
    l14 = (float)(D(MF(0x0057afe0)) - D(MF(0x0057afd8)));
    const double d = D(MF(0x0057afe4)) - D(MF(0x0057afdc));
    l10 = (float)d;
    l1c = (float)balance_ratio(d - D(l14), D(l10) + D(l14));
    // the aero (the CarData's downforce, front and rear)
    l14 = (float)(-D(MF(0x0057b4cc)));
    const double e = -D(MF(0x0057b4d0));
    l10 = (float)e;
    const double r3 = balance_ratio(e - D(l14), D(l10) + D(l14));
    l14 = (float)r3;
    l20 = (float)(((r3 * KF(0x3eaa7efau)) + (D(l1c) * KF(0x3eaa7efau))) + (D(l18) * KF(0x3eaa7efau)));
    int32_t ebp = self->w - 10;
    int32_t ebx = self->y + 0xb;
    const int32_t eax = ebp + 0x41;
    ebp = ebp - 0x41;
    const int32_t edi = eax / 2;
    style_draw(0x12, self->x + 10, ebx - 4, xl_text(0x005d5b70));
    {
        volatile int32_t w = ebp;                        // [esp+0x10] = ebp; fild; fst: the float of it kept there
        const double fw = D(w);
        *(volatile float*)&w = (float)fw;
        l10 = *(volatile float*)&w;
        balance_bar(self, ebx, edi, (fw * D(l18)) * KF(0x3f000000u), 0, *(volatile uint32_t*)&l18);
    }
    ebx += 10;
    style_draw(0x12, self->x + 10, ebx - 4, xl_text(0x005d5b80));
    balance_bar(self, ebx, edi, (D(l10) * D(l1c)) * KF(0x3f000000u), 0, *(volatile uint32_t*)&l1c);
    ebx += 10;
    style_draw(0x12, self->x + 10, ebx - 4, xl_text(0x005d5b90));
    balance_bar(self, ebx, edi, (D(l10) * D(l14)) * KF(0x3f000000u), 0, *(volatile uint32_t*)&l14);
    ebx += 0xc;
    style_draw(0x12, self->x + 10, ebx - 4, xl_text(0x005d5ba0));
    balance_bar(self, ebx, edi, (D(l10) * D(l20)) * KF(0x3f000000u), 0, *(volatile uint32_t*)&l20);
    {
        const int32_t y = self->y;
        const int32_t x = add32(self->x, edi);
        const uint32_t c = UI_GU32(M_C_57AEDC);
        const int32_t y1 = add32(self->h, y) - 5;
        line(x, y + 7, x, y1, c);
    }
}
static void fp_BalanceControl_Draw(Footprint& f, BalanceControl*, Edx, gxCanvas* c) {
    if (!fp_draw_base(f, c, M_ONCE_BALANCE, 0x0f)) return;
    f.add((void*)(uintptr_t)M_CARDATA, 0x1e0, "the CarData (CarFileCombine)");
    for (uint32_t xl : {0x005d5b70u, 0x005d5b80u, 0x005d5b90u, 0x005d5ba0u}) fp_xl(f, xl);
}
PORT_FN(0x00493d80, "BalanceControl::Draw", BalanceControl_Draw_n, fp_BalanceControl_Draw)

// =====================================================================================================================
// ChassisControl: springs, shocks, anti-roll bars (front / rear, numbers with arrows), a page per radio button
// =====================================================================================================================
static void __fastcall ChassisControl_Create_n(ChassisControl* self, Edx) {
    notify(self, 1, 0x0057af70, 4);
    notify(self, 1, 0x0057af80, 4);
    notify(self, 2, 0x0057af78, 4);
    notify(self, 2, 0x0057af88, 4);
    notify(self, 3, 0x0057af98, 4);
    notify(self, 5, 0x0057af90, 4);
    notify(self, 4, 0x0057af9c, 4);
    notify(self, 6, 0x0057af94, 4);
}
PORT_FN(0x00494380, "ChassisControl::Create", ChassisControl_Create_n, fp_create<ChassisControl>)

static void __fastcall ChassisControl_Callback_n(ChassisControl* self, Edx, int32_t id, const void*) {
    dirty(self);
    if (id >= 1) self->page = id;
}
PORT_FN(0x00494a10, "ChassisControl::Callback", ChassisControl_Callback_n, fp_callback<ChassisControl>)

// the page's title, from a table on the stack; pages 1 and 2 (springs, shocks) draw the shock graph and the front /
// rear frequencies, 3..6 a picture
// FIX CANDIDATE: a page outside 0..6 reads the title pointer from elsewhere in the frame (Callback only sets 1..6).
static void __fastcall ChassisControl_Draw_n(ChassisControl* self, Edx, gxCanvas* canvas) {
    struct {
        volatile float pts[4];                           // F+4 shock_fn's argument
        volatile float w2, y2, x2;                       // F+0x14, F+0x18, F+0x1c
        GraphInfo gi;                                    // F+0x20
        union { const char* volatile tbl[7]; char buf[0x50]; };   // F+0x44 (the sprintf buffer over the table)
    } fr;
    xl_once(M_ONCE_CHASSIS_DRAW, 1, 0x005d5b50, 0x004f9ea4, 0x00495a40);
    xl_once(M_ONCE_CHASSIS_DRAW, 2, 0x005d5b60, 0x004f9e88, 0x00495a30);
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp[0], self->x, self->y, 0);
    xl_once(M_ONCE_CHASSIS_DRAW, 0x04, 0x005d5af0, 0x004f9e6c, 0x00495a20);
    xl_once(M_ONCE_CHASSIS_DRAW, 0x08, 0x005d5b00, 0x004f9e50, 0x00495a10);
    xl_once(M_ONCE_CHASSIS_DRAW, 0x10, 0x005d5b10, 0x004f9e34, 0x00495a00);
    xl_once(M_ONCE_CHASSIS_DRAW, 0x20, 0x005d5b20, 0x004f9e18, 0x004959f0);
    xl_once(M_ONCE_CHASSIS_DRAW, 0x40, 0x005d5b40, 0x004f9dfc, 0x004959e0);
    xl_once(M_ONCE_CHASSIS_DRAW, 0x80, 0x005d5b30, 0x004f9ddc, 0x004959d0);
    fr.tbl[0] = (const char*)0x004f9dd4;                                              // "<HUH?>"
    fr.tbl[1] = xl_text(0x005d5af0);
    fr.tbl[2] = xl_text(0x005d5b00);
    fr.tbl[3] = xl_text(0x005d5b10);
    fr.tbl[4] = xl_text(0x005d5b20);
    fr.tbl[5] = xl_text(0x005d5b30);
    fr.tbl[6] = xl_text(0x005d5b40);
    style_draw(0x11, self->x + 0x5f, self->y + 0xa9, fr.tbl[self->page]);             // FIX CANDIDATE: page outside 0..6
    const int32_t page = self->page;
    if (page == 1 || page == 2) {
        stamp_at(self->stamp[1], self->x + 0xf, self->y + 5, 0);
        {
            const char* t = xl_text(0x005d5b50);
            const int32_t y = add32(self->y, self->h) - 0x2d;
            style_draw(0x11, add32(self->x, self->w) - 0x32, y, t);
        }
        {
            const char* t = xl_text(0x005d5b60);
            const int32_t y = add32(self->y, self->h) - 0x2d;
            style_draw(0x11, self->x + 0x32, y, t);
        }
        // the springs' rates and the car's masses: the front and rear natural frequencies
        const double k350 = KF(0x43af3264u);
        double x1 = ((D(MF(0x0057af70)) + D(MF(0x0057af80))) * KF(0x40000000u)) * KF(0x3e800000u);
        x1 = ((x1 * (D(MF(0x0057b17c)) - D(MF(0x0057b14c)))) + D(MF(0x0057b14c))) * k350;
        const double a2 = (D(MF(0x0057af88)) + D(MF(0x0057af78))) * KF(0x3f000000u);
        fr.x2 = (float)(((a2 * (D(MF(0x0057b180)) - D(MF(0x0057b150)))) + D(MF(0x0057b150))) * k350);
        const double y1 = (((D(MF(0x0057b174)) - D(MF(0x0057b144))) * D(MF(0x0057af98))) + D(MF(0x0057b144))) * k350;
        fr.y2 = (float)((((D(MF(0x0057b178)) - D(MF(0x0057b148))) * D(MF(0x0057af9c))) + D(MF(0x0057b148))) * k350);
        const double w1 = (((D(MF(0x0057b06c)) * KF(0x3c23d70au)) * D(MF(M_CARFILE))) * KF(0x3ee8b439u)) - KF(0x4235cccdu);
        fr.w2 = (float)(((((D(MF(0x0057b06c)) * KF(0xbc23d70au)) + KF(K_1)) * D(MF(M_CARFILE))) * KF(0x3ee8b439u)) - KF(0x4235cccdu));
        UI_sprintf(fr.buf, (const char*)0x004f9dc8, (x1 / x87_sqrt(y1 * w1)) * KF(0x3f000000u));      // "F: %1.2f"
        locale(fr.buf);
        UI_sprintf(fr.buf, (const char*)0x004f9dbc, (D(fr.x2) / x87_sqrt(D(fr.w2) * D(fr.y2))) * KF(0x3f000000u));  // "R: %1.2f"
        locale(fr.buf);
        {
            const int32_t x = self->x;
            const int32_t y = self->y;
            graph(&fr.gi, x + 0x28, y + 0x14, add32(self->w, x) - 0x28, add32(self->h, y) - 0x37, 0x28, 0xc1c80000u,
                  0x41c80000u, 0xc47a0000u, 0x447a0000u);
        }
        graph_axes(&fr.gi, 0, 0, UI_GU32(M_C_57AF2C));
        double v1, v2;
        if (self->page == 1) {
            v1 = ((D(MF(0x0057b17c)) - D(MF(0x0057b14c))) * D(MF(0x0057af70))) + D(MF(0x0057b14c));
            fr.pts[0] = (float)v1;
            fr.pts[1] = (float)v1;
            v2 = ((D(MF(0x0057b184)) - D(MF(0x0057b154))) * D(MF(0x0057af80))) + D(MF(0x0057b154));
        } else {
            v1 = ((D(MF(0x0057b180)) - D(MF(0x0057b150))) * D(MF(0x0057af78))) + D(MF(0x0057b150));
            fr.pts[0] = (float)v1;
            fr.pts[1] = (float)v1;
            v2 = ((D(MF(0x0057b188)) - D(MF(0x0057b158))) * D(MF(0x0057af88))) + D(MF(0x0057b158));
        }
        fr.pts[2] = (float)v2;
        fr.pts[3] = (float)v2;
        graph_plot(&fr.gi, F_shock_fn, (const void*)fr.pts, UI_GU32(M_C_57A734));
        return;
    }
    if (page == 3 || page == 4 || page == 5 || page == 6) stamp_at(self->stamp[2], self->x + 5, self->y + 5, 0);
}
static void fp_ChassisControl_Draw(Footprint& f, ChassisControl*, Edx, gxCanvas* c) {
    if (!fp_draw_base(f, c, M_ONCE_CHASSIS_DRAW, 0xff)) return;
    for (uint32_t xl : {0x005d5b50u, 0x005d5b60u, 0x005d5af0u, 0x005d5b00u, 0x005d5b10u, 0x005d5b20u, 0x005d5b30u, 0x005d5b40u}) fp_xl(f, xl);
}
PORT_FN(0x00494410, "ChassisControl::Draw", ChassisControl_Draw_n, fp_ChassisControl_Draw)

// the page's widgets: a picture, two headings, and four rows (springs, bump, rebound, anti-roll) of front / rear
// numbers with arrows -- a number and its arrows greyed out ("---", arrows off) where the car file's two limits are equal
static void __fastcall ChassisControl_Added_n(ChassisControl* self, Edx) {
    struct {
        volatile float t;                                // F+0xc
        UIDialogItem items[32];                          // F+0x10
    } fr;
    UIDialogItem* it = fr.items;
    xl_once(M_ONCE_CHASSIS_ADDED, 0x01, 0x005d5a90, 0x004f9f44, 0x004959c0);
    xl_once(M_ONCE_CHASSIS_ADDED, 0x02, 0x005d5aa0, 0x004f9f38, 0x004959b0);
    xl_once(M_ONCE_CHASSIS_ADDED, 0x04, 0x005d5ab0, 0x004f9f24, 0x004959a0);
    xl_once(M_ONCE_CHASSIS_ADDED, 0x08, 0x005d5ac0, 0x004f9f0c, 0x00495990);
    xl_once(M_ONCE_CHASSIS_ADDED, 0x10, 0x005d5ad0, 0x004f9ef4, 0x00495980);
    xl_once(M_ONCE_CHASSIS_ADDED, 0x20, 0x005d5ae0, 0x004f9edc, 0x00495970);
    it_ctor(&it[0], 0xe, 0, 0x23, 0xec, 0, 0, 0x004f9ecc, 0, 0, 0, 0, 0, 0, 0);                        // "gpshocks.stp"
    it_ctor(&it[1], 5, 0, 0xd9, 0x8c, 0, 0, P(xl_text(0x005d5a90)), 0, 0, 0x12, 0, 0, 0, 0);
    it_ctor(&it[2], 5, 0, 0x13d, 0x8c, 0, 0, P(xl_text(0x005d5aa0)), 0, 0, 0x12, 0, 0, 0, 0);
    // springs (y 0x98): front 0x57af70 (limits 0x57b14c / 0x57b17c), rear 0x57af78 (0x57b150 / 0x57b180)
    it_ctor(&it[3], 5, 0, 0x46, 0x98, 0, 0, P(xl_text(0x005d5ab0)), 0, 0, lstyle(), 0, 0, 0, 0);
    {
        const double d = absdiff(0x0057b14c, 0x0057b17c);
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[4], 6, 0, 0xd2, 0x98, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af70, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    it_ctor(&it[5], 0x10, 0, 0xbe, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b14c, 0x0057b17c)) ? 0 : 0x65, 0x0057af70, 0, 0, K_1, 0, 0);
    it_set(&it[6], 0x10, 0, 0x101, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b14c, 0x0057b17c)) ? 0 : 0x65, 0x0057af70, 1, 0, K_1, 0, 0);
    {
        const double d = absdiff(0x0057b150, 0x0057b180);
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[7], 6, 0, 0x136, 0x98, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af78, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    it_ctor(&it[8], 0x10, 0, 0x122, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b150, 0x0057b180)) ? 0 : 0x65, 0x0057af78, 0, 0, K_1, 0, 0);
    it_set(&it[9], 0x10, 0, 0x165, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b150, 0x0057b180)) ? 0 : 0x65, 0x0057af78, 1, 0, K_1, 0, 0);
    // bump (y 0xac): 0x57af80 (0x57b154 / 0x57b184), 0x57af88 (0x57b158 / 0x57b188)
    it_ctor(&it[10], 5, 0, 0x46, 0xac, 0, 0, P(xl_text(0x005d5ac0)), 0, 0, lstyle(), 0, 0, 0, 0);
    {
        const double d = absdiff(0x0057b154, 0x0057b184);
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[11], 6, 0, 0xd2, 0xac, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af80, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    it_ctor(&it[12], 0x10, 0, 0xbe, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b154, 0x0057b184)) ? 0 : 0x65, 0x0057af80, 0, 0, K_1, 0, 0);
    it_set(&it[13], 0x10, 0, 0x101, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b154, 0x0057b184)) ? 0 : 0x65, 0x0057af80, 1, 0, K_1, 0, 0);
    {
        const double d = absdiff(0x0057b158, 0x0057b188);
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[14], 6, 0, 0x136, 0xac, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af88, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    it_ctor(&it[15], 0x10, 0, 0x122, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b158, 0x0057b188)) ? 0 : 0x65, 0x0057af88, 0, 0, K_1, 0, 0);
    it_set(&it[16], 0x10, 0, 0x165, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b158, 0x0057b188)) ? 0 : 0x65, 0x0057af88, 1, 0, K_1, 0, 0);
    // rebound (y 0xc0): 0x57af98 (0x57b144 / 0x57b174), 0x57af9c (0x57b148 / 0x57b178)
    it_ctor(&it[17], 5, 0, 0x46, 0xc0, 0, 0, P(xl_text(0x005d5ad0)), 0, 0, lstyle_r(), 0, 0, 0, 0);
    {
        const double d = absdiff(0x0057b144, 0x0057b174);
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[18], 6, 0, 0xd2, 0xc0, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af98, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    it_ctor(&it[19], 0x10, 0, 0xbe, 0xc0, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b144, 0x0057b174)) ? 0 : 0x65, 0x0057af98, 0, 0, K_1, 0, 0);
    it_set(&it[20], 0x10, 0, 0x101, 0xc0, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b144, 0x0057b174)) ? 0 : 0x65, 0x0057af98, 1, 0, K_1, 0, 0);
    {
        const double d = absdiff(0x0057b148, 0x0057b178);
        const bool a = near_reg(d), c = near_bits(d);
        fr.t = (float)d;
        it_ctor(&it[21], 6, 0, 0x136, 0xc0, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af9c, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    {
        const double d = absdiff(0x0057b148, 0x0057b178);
        fr.t = (float)d;
        it_set(&it[22], 0x10, 0, 0x122, 0xc0, 0, 0, T_EMPTY, near_reg(d) ? 0 : 0x65, 0x0057af9c, 0, 0, K_1, 0, 0);
        it_ctor(&it[23], 0x10, 0, 0x165, 0xc0, 0, 0, T_EMPTY, near_bits(d) ? 0 : 0x65, 0x0057af9c, 1, 0, K_1, 0, 0);
    }
    // anti-roll (y 0xd4): 0x57af90 (0x57b15c / 0x57b18c), 0x57af94 (0x57b160 / 0x57b190)
    it_ctor(&it[24], 5, 0, 0x46, 0xd4, 0, 0, P(xl_text(0x005d5ae0)), 0, 0, lstyle(), 0, 0, 0, 0);
    {
        const double d = absdiff(0x0057b15c, 0x0057b18c);
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[25], 6, 0, 0xd2, 0xd4, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af90, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    {
        const double d = absdiff(0x0057b15c, 0x0057b18c);
        it_set(&it[26], 0x10, 0, 0xbe, 0xd4, 0, 0, T_EMPTY, near_reg(d) ? 0 : 0x65, 0x0057af90, 0, 0, K_1, 0, 0);
        it_ctor(&it[27], 0x10, 0, 0x101, 0xd4, 0, 0, T_EMPTY, near_bits(d) ? 0 : 0x65, 0x0057af90, 1, 0, K_1, 0, 0);
    }
    {
        const double d = absdiff(0x0057b160, 0x0057b190);
        const bool a = near_reg(d), c = near_bits(d);
        it_set(&it[28], 6, 0, 0x136, 0xd4, 0, 0, c ? T_DASH : T_F10, 0, 0x0057af94, a ? 0x16 : 0x15, K_100, 0, 0, 0);
        it_set(&it[29], 0x10, 0, 0x122, 0xd4, 0, 0, T_EMPTY, c ? 0 : 0x65, 0x0057af94, 0, 0, K_1, 0, 0);
        it_ctor(&it[30], 0x10, 0, 0x165, 0xd4, 0, 0, T_EMPTY, c ? 0 : 0x65, 0x0057af94, 1, 0, K_1, 0, 0);
    }
    it_end(&it[31]);
    add_items(self, fr.items);
}
PORT_FN(0x00494a30, "ChassisControl::Added", ChassisControl_Added_n, fp_added<ChassisControl>)

// =====================================================================================================================
// AlignControl: camber, toe, ride height, brake bias, wheel lock (pictures and angles), and a tire's force curves
// =====================================================================================================================
static void __fastcall AlignControl_Create_n(AlignControl* self, Edx) {
    self->page = 1;
    notify(self, 0, M_SETUP, 0x8c);
    notify(self, 1, 0x0057afe0, 4);
    notify(self, 2, 0x0057afe4, 4);
    notify(self, 3, 0x0057afd8, 4);
    notify(self, 4, 0x0057afdc, 4);
    notify(self, 8, 0x0057afe8, 4);
    notify(self, 7, 0x0057af68, 4);
}
PORT_FN(0x00495aa0, "AlignControl::Create", AlignControl_Create_n, fp_create<AlignControl>)

static void __fastcall AlignControl_Callback_n(AlignControl* self, Edx, int32_t id, const void*) {
    if (id != 0) self->page = id;
    dirty(self);
}
PORT_FN(0x00495b20, "AlignControl::Callback", AlignControl_Callback_n, fp_callback<AlignControl>)

// fcos / fsin of a float, the full-precision result multiplied (rounded) -- x87_cos_mul / x87_sin_mul -- and, for the
// `fld a; fsin; fst [a]; fmul k` sequences, the sine also stored back to the float
static __forceinline double sin_store_mul(volatile float* a, double k) {
    const float x = *a;
    float s;
    double r;
    __asm { fld x
            fsin
            fst s
            fmul k
            fstp r }
    *a = s;
    return r;
}
// fild n; fld a; fcos; fmul st(1) -> cos(a) * n (the n left in st(1): the caller's next fsin uses it too)
static __forceinline double cos_mul_i(float a, int32_t n) {
    double r;
    __asm { fild n
            fld a
            fcos
            fmul st, st(1)
            fstp r
            fstp st(0) }
    return r;
}
// fld a; fsin; fmulp st(1) with the n from fild, the product stored (fst) and returned
static __forceinline double sin_mul_i(float a, int32_t n) {
    double r;
    __asm { fild n
            fld a
            fsin
            fmulp st(1), st
            fstp r }
    return r;
}

// the page's title from a table on the stack, then its picture and figures
// FIX CANDIDATE: a page outside 0..9 reads the title pointer from elsewhere in the frame (Callback sets any nonzero id:
// 1..4, 7, 8 from the notifications; 9 has no notification).
static void __fastcall AlignControl_Draw_n(AlignControl* self, Edx, gxCanvas* canvas) {
    struct {
        volatile int32_t yoff;                           // F+0x10
        volatile float a14;                              // F+0x14 (an angle, then an int, then a product)
        GraphInfo gi;                                    // F+0x14 on the tire page (overlapping a14)
        TireArg targ;                                    // F+0x3c
        union { const char* volatile tbl[10]; char buf[0x28]; };   // F+0x48
        volatile float a70;                              // F+0x70
        volatile uint32_t curves[8];                     // F+0x70 on the tire page: {load, colour} x 3, then -1.0
    } fr;
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp[0], self->x, self->y, 0);
    xl_once(M_ONCE_ALIGN_DRAW, 1, 0x005d5a70, 0x004f9f84, 0x00497430);
    xl_once(M_ONCE_ALIGN_DRAW, 2, 0x005d5a80, 0x004f9f6c, 0x00497420);
    fr.tbl[0] = (const char*)0x004f9dd4;                                              // "<HUH?>"
    fr.tbl[1] = xl_text(0x0057b2b8);
    fr.tbl[2] = xl_text(0x0057b278);
    fr.tbl[3] = xl_text(0x0057b6d0);
    fr.tbl[4] = xl_text(0x0057b748);
    fr.tbl[5] = xl_text(0x0057b6a0);
    fr.tbl[6] = xl_text(0x0057b6f8);
    fr.tbl[7] = xl_text(0x005d5a70);
    fr.tbl[8] = xl_text(0x005d5a80);
    fr.tbl[9] = xl_text(0x0057b630);
    style_draw(0x11, self->x + 0x5f, self->y + 0xa9, fr.tbl[self->page]);             // FIX CANDIDATE: page outside 0..9
    const int32_t page = self->page;
    const uint32_t c_on = M_C_57AEDC, c_axis = M_C_57AF2C;
    if (page == 3 || page == 4) {
        // camber: a wheel leaning by the angle, both sides
        stamp_at(page == 3 ? self->stamp[1] : self->stamp[2], self->x + 3, self->y + 5, 0);
        double v;
        if (self->page == 3) v = ((D(MF(0x0057b194)) - D(MF(0x0057b164))) * D(MF(0x0057afd8))) + D(MF(0x0057b164));
        else v = ((D(MF(0x0057b198)) - D(MF(0x0057b168))) * D(MF(0x0057afdc))) + D(MF(0x0057b168));
        fr.a70 = (float)v;
        fr.a14 = (float)((D(fr.a70) * KF(0x3c8efa35u)) * KF(0x40800000u));
        UI_sprintf(fr.buf, (const char*)0x004f9f64, D(fr.a70), (int32_t)-0x50);        // "%1.1f%c" (a degree sign)
        style_draw(0x11, self->x + 0x1e, self->y + 0x8c, fr.buf);
        style_draw(0x11, self->x + 0x9b, self->y + 0x8c, fr.buf);
        {
            const int32_t y = self->y, x = self->x + 0x1e;
            line(x, y + 0x5a, x, y + 0x82, UI_GU32(c_axis));
        }
        const int32_t edi = x87_ftol(x87_cos_mul(D(fr.a14), KF(0x42200000u)) + KF(0x3f000000u));
        const int32_t tb = x87_ftol(sin_store_mul(&fr.a14, KF(0x42200000u)) + KF(0x3f000000u));
        {
            const int32_t y = self->y, x = self->x;
            line(x + 0x1e, y + 0x5a, add32(tb, x) + 0x1e, add32(y, edi) + 0x5a, UI_GU32(c_on));
        }
        {
            const int32_t y = self->y, x = self->x + 0x9b;
            line(x, y + 0x5a, x, y + 0x82, UI_GU32(c_axis));
        }
        {
            const int32_t y = self->y, x = self->x;
            const uint32_t c = UI_GU32(c_on);
            const int32_t tc = x87_ftol((D(fr.a14) * KF(0xc2200000u)) - KF(0x3f000000u));
            line(x + 0x9b, y + 0x5a, add32(tc, x) + 0x9b, add32(y, edi) + 0x5a, c);
        }
        return;
    }
    if (page == 1 || page == 2) {
        // toe: a wheel seen from above, turned by the angle
        stamp_at(page == 1 ? self->stamp[3] : self->stamp[4], self->x + 2, self->y + 5, 0);
        const int32_t pg = self->page;
        double v;
        if (pg == 1) v = ((D(MF(0x0057b19c)) - D(MF(0x0057b16c))) * D(MF(0x0057afe0))) + D(MF(0x0057b16c));
        else v = ((D(MF(0x0057b1a0)) - D(MF(0x0057b170))) * D(MF(0x0057afe4))) + D(MF(0x0057b170));
        fr.a14 = (float)v;
        fr.a70 = (float)((D(fr.a14) * KF(0x3c8efa35u)) * KF(0x41200000u));
        const int32_t ebp = 0x2f;
        fr.yoff = 0x91;
        const int32_t edi = pg == 1 ? 0x50 : 0x4d;
        const int32_t ebx = 0x1e;
        UI_sprintf(fr.buf, (const char*)0x004f9f64, D(fr.a14), (int32_t)-0x50);
        style_draw(0x11, self->x + ebp, add32(self->y - ebx, edi) - 0xa, fr.buf);
        style_draw(0x11, add32(self->x, fr.yoff), add32(self->y - ebx, edi) - 0xa, fr.buf);
        {
            const int32_t y = self->y, x = self->x + ebp;
            line(x, add32(y, edi), x, add32(y - ebx, edi), UI_GU32(c_axis));
        }
        *(volatile int32_t*)&fr.a14 = ebx;
        const int32_t ta = x87_ftol(KF(0xbf000000u) - cos_mul_i(fr.a70, *(volatile int32_t*)&fr.a14));
        volatile int32_t ta_s = ta;                                                   // [esp+0x3c]
        {
            const uint32_t c = UI_GU32(c_on);
            const int32_t y1 = add32(add32(self->y, ta_s), edi);
            const double p = sin_mul_i(fr.a70, ebx);
            fr.a14 = (float)p;
            const int32_t tb = x87_ftol(p + KF(0x3f000000u));
            const int32_t x = self->x;
            line(x + ebp, add32(self->y, edi), add32(add32(x, tb), ebp), y1, c);
        }
        {
            const int32_t y = self->y;
            const int32_t x = add32(self->x, fr.yoff);
            line(x, add32(y, edi), x, add32(y - ebx, edi), UI_GU32(c_axis));
        }
        {
            const int32_t y = self->y, x = self->x;
            const uint32_t c = UI_GU32(c_on);
            const int32_t y1 = add32(add32(ta_s, y), edi);
            const int32_t tc = x87_ftol(KF(0xbf000000u) - D(fr.a14));
            line(add32(x, fr.yoff), add32(edi, y), add32(add32(tc, x), fr.yoff), y1, c);
        }
        return;
    }
    if (page == 8) {
        // wheel lock: the steering's limit, both wheels
        stamp_at(self->stamp[3], self->x + 2, self->y + 5, 0);
        const double v = D(MF(0x0057afe8)) * D(MF(0x0057b1d4));
        fr.a14 = (float)(KF(0x3c8efa35u) * v);
        UI_sprintf(fr.buf, (const char*)0x004f9f64, v, (int32_t)-0x50);
        style_draw(0x11, self->x + 0x2f, self->y + 0x28, fr.buf);
        style_draw(0x11, self->x + 0x91, self->y + 0x28, fr.buf);
        {
            const int32_t y = self->y, x = self->x + 0x2f;
            line(x, y + 0x50, x, y + 0x32, UI_GU32(c_axis));
        }
        const int32_t ebx = x87_ftol(x87_cos_mul(D(fr.a14), KF(0xc1f00000u)) - KF(0x3f000000u));
        const int32_t y0 = self->y;
        const int32_t ebp = x87_ftol(sin_store_mul(&fr.a14, KF(0x41f00000u)) + KF(0x3f000000u));
        {
            const int32_t x = self->x;
            line(x + 0x2f, y0 + 0x50, add32(x, ebp) + 0x2f, add32(y0, ebx) + 0x50, UI_GU32(c_on));
        }
        const int32_t edi = x87_ftol((D(fr.a14) * KF(0xc1f00000u)) - KF(0x3f000000u));
        {
            const int32_t x = self->x;
            const uint32_t c = UI_GU32(c_on);
            line(x + 0x2f, self->y + 0x50, add32(x, edi) + 0x2f, add32(self->y, ebx) + 0x50, c);
        }
        {
            const int32_t y = self->y, x = self->x + 0x91;
            line(x, y + 0x50, x, y + 0x32, UI_GU32(c_axis));
        }
        {
            const int32_t y = self->y, x = self->x;
            line(x + 0x91, y + 0x50, add32(x, ebp) + 0x91, add32(y, ebx) + 0x50, UI_GU32(c_on));
        }
        {
            const int32_t y = self->y, x = self->x;
            line(x + 0x91, y + 0x50, add32(x, edi) + 0x91, add32(y, ebx) + 0x50, UI_GU32(c_on));
        }
        return;
    }
    if (page == 7) {
        // brake bias: a marker along a bar
        stamp_at(self->stamp[5], self->x + 0x14, self->y + 0x28, 0);
        {
            const int32_t y = self->y + 0x4f, x = self->x + 0x2c;
            line(x, y, x + 0x5a, y, UI_GU32(c_on));
        }
        const int32_t t = x87_ftol((D(MF(0x0057af68)) * KF(0x42b40000u)) + KF(0x42300000u));
        const int32_t y = self->y + 0x4f;
        const int32_t x = add32(self->x, t);
        line(x, y, x, y + 5, UI_GU32(c_on));
        return;
    }
    if (page != 9) return;
    // the tire's lateral force against slip angle at three loads (a legend of their colours), for the front camber
    {
        const int32_t x = self->x, y = self->y;
        fr.gi.x0 = x + 0xc;
        fr.gi.y0 = y + 0x14;
        fr.gi.x1 = add32(self->w, x) - 0xc;
        fr.gi.y1 = add32(self->h, y) - 0x32;
    }
    fr.gi.tick = 0x28;
    const uint32_t f1e0 = UI_GU32(0x0057b1e0), f1dc = UI_GU32(0x0057b1dc);
    fr.gi.xlo = 0xc1200000u;
    fr.gi.xhi = 0x41200000u;
    fr.gi.ylo = 0;
    const uint32_t f1d8 = UI_GU32(0x0057b1d8);
    fr.gi.yhi = 0x44e10000u;
    void* tire = ccall<void*>(F_TireCreate, f1d8, f1dc, f1e0, (const char*)0x004f9f54);   // "def_tire.tir"
    const double camber = ((D(MF(0x0057b194)) - D(MF(0x0057b164))) * D(MF(0x0057afd8))) + D(MF(0x0057b164));
    volatile void* tire2 = tire;                                                      // [esp+0x38]
    fr.targ.tire = tire;
    fr.targ.camber = (float)camber;
    graph_axes(&fr.gi, 5, 6, UI_GU32(c_axis));
    fr.curves[0] = 0x44610000u;                                                       // 900
    fr.curves[1] = UI_GU32(M_C_57B6F0);
    fr.curves[2] = 0x44a8c000u;                                                       // 1350
    fr.curves[3] = UI_GU32(M_C_57B6AC);
    fr.curves[4] = 0x44e10000u;                                                       // 1800
    fr.curves[5] = UI_GU32(M_C_57A7C8);
    fr.curves[6] = 0xbf800000u;                                                       // -1: the end
    fr.curves[7] = 0;
    if ((int32_t)fr.curves[0] > 0) {
        fr.yoff = 0x50;
        volatile uint32_t* e = fr.curves;
        do {
            const uint32_t load = e[0], colour = e[1];
            e += 2;
            *(volatile uint32_t*)&fr.targ.load = load;
            graph_plot(&fr.gi, F_tire_fn, (const void*)&fr.targ, colour);
            const int32_t bx = self->x + 0x96;
            const int32_t by = add32(self->y, fr.yoff);
            rect(bx, by, bx + 0xa, by + 8, e[-1]);
            UI_sprintf(fr.buf, (const char*)0x004f9ec4, D(fr.targ.load));             // "%1.0f"
            style_draw(0x14, bx - 5, by, fr.buf);
            fr.yoff = fr.yoff + 0xa;
        } while ((int32_t)e[0] > 0);
    }
    ccall<void>(F_TireDestroy, (void*)tire2);
}
static void fp_AlignControl_Draw(Footprint& f, AlignControl* self, Edx, gxCanvas* c) {
    if (!fp_draw_base(f, c, M_ONCE_ALIGN_DRAW, 3)) return;
    if (self->page == 9) { f.replay_only = "the tire page: TireCreate / TireDestroy (allocates, loads the tire)"; return; }
    for (uint32_t xl : {0x005d5a70u, 0x005d5a80u, 0x0057b2b8u, 0x0057b278u, 0x0057b6d0u, 0x0057b748u, 0x0057b6a0u, 0x0057b6f8u, 0x0057b630u})
        fp_xl(f, xl);
}
PORT_FN(0x00495b40, "AlignControl::Draw", AlignControl_Draw_n, fp_AlignControl_Draw)

// the page's widgets: a picture, two headings, rows of front / rear numbers (in the car file's units: its two limits
// as the number's scale and offset) with arrows -- toe, camber, ride height -- and the brake bias and wheel lock
static void __fastcall AlignControl_Added_n(AlignControl* self, Edx) {
    struct { UIDialogItem items[36]; } fr;               // F+0x10
    UIDialogItem* it = fr.items;
    xl_once(M_ONCE_ALIGN_ADDED, 0x01, 0x005d5a00, 0x004f9f44, 0x00497410);
    xl_once(M_ONCE_ALIGN_ADDED, 0x02, 0x005d5a10, 0x004f9f38, 0x00497400);
    xl_once(M_ONCE_ALIGN_ADDED, 0x04, 0x005d5a20, 0x004fa00c, 0x004973f0);
    xl_once(M_ONCE_ALIGN_ADDED, 0x08, 0x005d5a30, 0x004f9ff4, 0x004973e0);
    xl_once(M_ONCE_ALIGN_ADDED, 0x10, 0x005d5a40, 0x004f9fdc, 0x004973d0);
    xl_once(M_ONCE_ALIGN_ADDED, 0x20, 0x005d5a50, 0x004f9fc4, 0x004973c0);
    xl_once(M_ONCE_ALIGN_ADDED, 0x40, 0x005d5a60, 0x004f9fac, 0x004973b0);
    it_ctor(&it[0], 0xe, 0, 0x23, 0xec, 0, 0, 0x004f9ecc, 0, 0, 0, 0, 0, 0, 0);
    it_ctor(&it[1], 5, 0, 0xd9, 0x8c, 0, 0, P(xl_text(0x005d5a00)), 0, 0, 0x12, 0, 0, 0, 0);
    it_ctor(&it[2], 5, 0, 0x13d, 0x8c, 0, 0, P(xl_text(0x005d5a10)), 0, 0, 0x12, 0, 0, 0, 0);
    // a row's number: scale = hi - lo (stored), offset = lo (its bits), the style by whether the limits differ
    auto number = [&](UIDialogItem* p, uint32_t x, uint32_t y, uint32_t fmt, uint32_t var, uint32_t lo, uint32_t hi) {
        const bool n = near_reg(absdiff(lo, hi));
        const float scale = (float)(D(MF(hi)) - D(MF(lo)));
        const uint32_t off = UI_GU32(lo);
        it_ctor(p, 6, 0, x, y, 0, 0, fmt, 0, var, n ? 0x16 : 0x15, f_bits(scale), off, 0, 0);
    };
    // toe (y 0x98): 0x57afe0 (0x57b16c / 0x57b19c), 0x57afe4 (0x57b170 / 0x57b1a0)
    it_ctor(&it[3], 5, 0, 0x46, 0x98, 0, 0, P(xl_text(0x005d5a20)), 0, 0, lstyle(), 0, 0, 0, 0);
    number(&it[4], 0xd2, 0x98, 0x004f9fa4, 0x0057afe0, 0x0057b16c, 0x0057b19c);                          // "%1.1f"
    it_ctor(&it[5], 0x10, 0, 0xbe, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b16c, 0x0057b19c)) ? 0 : 0x51, 0x0057afe0, 0, 0, K_1, 0, 0);
    it_ctor(&it[6], 0x10, 0, 0x101, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b16c, 0x0057b19c)) ? 0 : 0x51, 0x0057afe0, 1, 0, K_1, 0, 0);
    number(&it[7], 0x136, 0x98, 0x004f9fa4, 0x0057afe4, 0x0057b170, 0x0057b1a0);
    it_ctor(&it[8], 0x10, 0, 0x122, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b170, 0x0057b1a0)) ? 0 : 0x51, 0x0057afe4, 0, 0, K_1, 0, 0);
    it_ctor(&it[9], 0x10, 0, 0x165, 0x98, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b170, 0x0057b1a0)) ? 0 : 0x51, 0x0057afe4, 1, 0, K_1, 0, 0);
    // camber (y 0xac): 0x57afd8 (0x57b164 / 0x57b194), 0x57afdc (0x57b168 / 0x57b198)
    it_ctor(&it[10], 5, 0, 0x46, 0xac, 0, 0, P(xl_text(0x005d5a30)), 0, 0, lstyle_r(), 0, 0, 0, 0);
    number(&it[11], 0xd2, 0xac, 0x004f9fa4, 0x0057afd8, 0x0057b164, 0x0057b194);
    it_ctor(&it[12], 0x10, 0, 0xbe, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b164, 0x0057b194)) ? 0 : 0x51, 0x0057afd8, 0, 0, K_1, 0, 0);
    it_set(&it[13], 0x10, 0, 0x101, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b164, 0x0057b194)) ? 0 : 0x51, 0x0057afd8, 1, 0, K_1, 0, 0);
    {
        const bool n = near_reg(absdiff(0x0057b168, 0x0057b198));
        const float scale = (float)(D(MF(0x0057b198)) - D(MF(0x0057b168)));
        const uint32_t off = UI_GU32(0x0057b168);
        it_ctor(&it[14], 6, 0, 0x136, 0xac, 0, 0, 0x004f9fa4, 0, 0x0057afdc, n ? 0x16 : 0x15, f_bits(scale), off, 0, 0);
    }
    it_ctor(&it[15], 0x10, 0, 0x122, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b168, 0x0057b198)) ? 0 : 0x51, 0x0057afdc, 0, 0, K_1, 0, 0);
    it_set(&it[16], 0x10, 0, 0x165, 0xac, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b168, 0x0057b198)) ? 0 : 0x51, 0x0057afdc, 1, 0, K_1, 0, 0);
    // ride height (y 0xc0): 0x57afa0 (0x57b058 / 0x57b05c), 0x57afa4 (0x57b060 / 0x57b064)
    it_ctor(&it[17], 5, 0, 0x46, 0xc0, 0, 0, P(xl_text(0x005d5a40)), 0, 0, lstyle(), 0, 0, 0, 0);
    number(&it[18], 0xd2, 0xc0, 0x004f9f9c, 0x0057afa0, 0x0057b058, 0x0057b05c);                        // "%1.1f\""
    it_ctor(&it[19], 0x10, 0, 0xbe, 0xc0, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b058, 0x0057b05c)) ? 0 : 0x51, 0x0057afa0, 0, 0, K_1, 0, 0);
    it_set(&it[20], 0x10, 0, 0x101, 0xc0, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b058, 0x0057b05c)) ? 0 : 0x51, 0x0057afa0, 1, 0, K_1, 0, 0);
    {
        const bool n = near_reg(absdiff(0x0057b060, 0x0057b064));
        const float scale = (float)(D(MF(0x0057b064)) - D(MF(0x0057b060)));
        const uint32_t off = UI_GU32(0x0057b060);
        it_ctor(&it[21], 6, 0, 0x136, 0xc0, 0, 0, 0x004f9f9c, 0, 0x0057afa4, n ? 0x16 : 0x15, f_bits(scale), off, 0, 0);
    }
    it_ctor(&it[22], 0x10, 0, 0x122, 0xc0, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b060, 0x0057b064)) ? 0 : 0x51, 0x0057afa4, 0, 0, K_1, 0, 0);
    it_set(&it[23], 0x10, 0, 0x165, 0xc0, 0, 0, T_EMPTY, near_reg(absdiff(0x0057b060, 0x0057b064)) ? 0 : 0x51, 0x0057afa4, 1, 0, K_1, 0, 0);
    // brake bias (y 0xd4): 0x57af68, a percentage (-100: front 100 .. rear 100)
    it_ctor(&it[24], 5, 0, 0x46, 0xd4, 0, 0, P(xl_text(0x005d5a50)), 0, 0, lstyle_r(), 0, 0, 0, 0);
    it_ctor(&it[25], 6, 0, 0xd2, 0xd4, 0, 0, T_F10, 0, 0x0057af68, 0x15, 0xc2c80000u, K_100, 0, 0);
    it_ctor(&it[26], 0x10, 0, 0xbe, 0xd4, 0, 0, T_EMPTY, 0x65, 0x0057af68, 0, 0, K_1, 0, 0);
    it_set(&it[27], 0x10, 0, 0x101, 0xd4, 0, 0, T_EMPTY, 0x65, 0x0057af68, 1, 0, K_1, 0, 0);
    it_ctor(&it[28], 6, 0, 0x136, 0xd4, 0, 0, T_F10, 0, 0x0057af68, 0x15, K_100, 0, 0, 0);
    it_ctor(&it[29], 0x10, 0, 0x122, 0xd4, 0, 0, T_EMPTY, 0x65, 0x0057af68, 0, 0, K_1, 0, 0);
    it_set(&it[30], 0x10, 0, 0x165, 0xd4, 0, 0, T_EMPTY, 0x65, 0x0057af68, 1, 0, K_1, 0, 0);
    // wheel lock (y 0xe8): 0x57afe8 times the car file's lock (0x57b1d4)
    it_ctor(&it[31], 5, 0, 0x46, 0xe8, 0, 0, P(xl_text(0x005d5a60)), 0, 0, lstyle(), 0, 0, 0, 0);
    it_ctor(&it[32], 6, 0, 0xd2, 0xe8, 0, 0, T_F10, 0, 0x0057afe8, 0x15, UI_GU32(0x0057b1d4), 0, 0, 0);
    it_ctor(&it[33], 0x10, 0, 0xbe, 0xe8, 0, 0, T_EMPTY, 0x1d, 0x0057afe8, 0, 0x3e4ccccdu, K_1, 0, 0);
    it_set(&it[34], 0x10, 0, 0x101, 0xe8, 0, 0, T_EMPTY, 0x1d, 0x0057afe8, 1, 0x3e4ccccdu, K_1, 0, 0);
    it_end(&it[35]);
    add_items(self, fr.items);
}
PORT_FN(0x00496510, "AlignControl::Added", AlignControl_Added_n, fp_added<AlignControl>)

// =====================================================================================================================
// TorqueCurveControl: the engine's power and torque curves (a PowerCurve set up from the car file)
// =====================================================================================================================
static void __fastcall TorqueCurveControl_Draw_n(TorqueCurveControl* self, Edx, gxCanvas* canvas) {
    struct {
        PowerArg arg;                                    // F+8
        GraphInfo gi;                                    // F+0x1c
        volatile uint32_t curve[6];                      // F+0x40 PowerCurve (its Setup's)
        char buf[0x100];                                 // F+0x58
    } fr;
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp, self->x, self->y, 0);
    {
        const float peak = (float)(D(MF(0x0057b08c)) * KF(0x3fadba5eu));
        typedef void(__fastcall * Setup_t)(volatile uint32_t*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        const uint32_t a = UI_GU32(0x0057b3b4), b = UI_GU32(0x0057b3bc), c = UI_GU32(0x0057b3b0), d = UI_GU32(0x0057b3b8);
        ((Setup_t)(uintptr_t)0x00446590)(fr.curve, 0, a, b, c, d, f_bits(peak));      // PowerCurve::Setup
    }
    {
        const int32_t x = self->x, y = self->y;
        fr.gi.x0 = x + 0x28;
        fr.gi.y0 = y + 0xa;
        fr.gi.x1 = add32(self->w, x) - 0xf;
        fr.gi.tick = 0x28;
        fr.gi.xlo = 0;
        fr.gi.ylo = 0;
        fr.gi.xhi = 0x40e00000u;                                                      // 7 (thousand rpm)
        fr.gi.yhi = 0x447a0000u;                                                      // 1000
        fr.arg.power = 1;
        fr.arg.curve = (void*)fr.curve;
        fr.gi.y1 = add32(self->h, y) - 0x11;
    }
    UI_sprintf(fr.buf, (const char*)0x004fa038, D(MF(0x0057b3b4)), D(MF(0x0057b3bc)));   // "%1.0f hp @ %1.0f RPM"
    style_draw(0x14, self->x + 0xa8, self->y + 0xa, fr.buf);
    UI_sprintf(fr.buf, (const char*)0x004fa020, D(MF(0x0057b3b0)), D(MF(0x0057b3b8)));   // "%1.0f lb-ft @ %1.0f RPM"
    style_draw(0x14, self->x + 0xa8, self->y + 0x14, fr.buf);
    graph_axes(&fr.gi, 7, 4, UI_GU32(M_C_57AF2C));
    *(volatile uint32_t*)&fr.arg.limit = UI_GU32(0x0057b084);                         // (mov: the bits)
    *(volatile uint32_t*)&fr.arg.rpm_scale = 0x447a0000u;                              // 1000
    *(volatile uint32_t*)&fr.arg.scale = 0x3a83126fu;                                  // 0.001
    graph_plot(&fr.gi, F_power_fn, (const void*)&fr.arg, UI_GU32(M_C_57B6F0));
    *(volatile uint32_t*)&fr.arg.scale = K_1;
    fr.arg.power = 0;
    graph_plot(&fr.gi, F_power_fn, (const void*)&fr.arg, UI_GU32(M_C_57A7C8));
}
static void fp_TorqueCurveControl_Draw(Footprint& f, TorqueCurveControl*, Edx, gxCanvas* c) { fp_draw_base(f, c, 0, 0); }
PORT_FN(0x004974b0, "TorqueCurveControl::Draw", TorqueCurveControl_Draw_n, fp_TorqueCurveControl_Draw)

// =====================================================================================================================
// DrivetrainControl: the six gears' ratios and the final drive (numbers with arrows), each gear's top speed
// =====================================================================================================================
static void __fastcall DrivetrainControl_Create_n(DrivetrainControl* self, Edx) {
    notify(self, 6, M_SETUP, 4);
    notify(self, 0, 0x0057afb0, 4);
    notify(self, 1, 0x0057afb4, 4);
    notify(self, 2, 0x0057afb8, 4);
    notify(self, 3, 0x0057afbc, 4);
    notify(self, 4, 0x0057afc0, 4);
    notify(self, 5, 0x0057afc4, 4);
    if (UI_G32(M_57A7C4) < 6) ccall<void>(F_UIHideGroup, (uint32_t)self->group6);
}
PORT_FN(0x00497d70, "DrivetrainControl::Create", DrivetrainControl_Create_n, fp_create<DrivetrainControl>)

static void __fastcall DrivetrainControl_Callback_n(DrivetrainControl* self, Edx, int32_t id, const void*) {
    self->page = id;
    dirty(self);
}
PORT_FN(0x00497e00, "DrivetrainControl::Callback", DrivetrainControl_Callback_n, fp_callback<DrivetrainControl>)

static void __fastcall DrivetrainControl_Draw_n(DrivetrainControl* self, Edx, gxCanvas* canvas) {
    struct {
        volatile uint32_t ratio;                         // F+4
        volatile float speed;                            // F+8
        char buf[0x20];                                  // F+0xc
    } fr;
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp[0], self->x, self->y, 0);
    stamp_at(self->stamp[1], self->x + 0xf, self->y + 7, self->page);
    // each gear at least 0.04 below the one before it
    int32_t n = UI_G32(M_CARFILE_108) - 1;
    // FIX: this ran for the car file's gear count (0x57b140), so a car file with more than 6 gears read and raised the
    // values after the setup's six ratios (the rest of the setup, then past it; a huge count wrapped the address): only
    // the six are kept apart. The garage shows and edits six gears in any case.
    if (VP_FIX && n > 5) n = 5;
    if (n > 0) {
        const double gap = KF(0x3d23d70au);
        uint32_t p = (uint32_t)n * 4u + 0x0057afb0u;
        do {
            const double v = D(UI_GF(p)) + gap;
            volatile float t = (float)v;
            if (v >= D(UI_GF(p - 4))) UI_GU32(p - 4) = *(volatile uint32_t*)&t;           // (fcom; test ah, 1: NaN keeps it)
            p -= 4;
        } while (p != 0x0057afb0u);
    }
    // the top speed in each gear: the engine's rpm limit through the gear, the final drive and the tire
    static const struct { int32_t gear; uint32_t ratio, setup; int32_t dx, dy; } g[6] = {
        {1, 0x0057b120, 0x0057afb0, 0x20, 0xe}, {3, 0x0057b128, 0x0057afb8, 0x4a, 0xe}, {5, 0x0057b130, 0x0057afc0, 0x74, 0xe},
        {2, 0x0057b124, 0x0057afb4, 0x20, 0x8c}, {4, 0x0057b12c, 0x0057afbc, 0x4a, 0x8c}, {6, 0x0057b134, 0x0057afc4, 0x74, 0x8c},
    };
    for (int k = 0; k < 6; k++) {
        if (UI_G32(M_57A7C4) < g[k].gear) continue;
        const double a = (D(MF(0x0057b1e8)) * D(MF(0x0057b1e4))) * KF(0x3727c5adu);
        const double b = D(MF(0x0057b1ec)) * KF(0x3c5013a9u);
        const uint32_t loc = UI_GU32(0x00509354);                                     // the LocaleInfo
        const double ab = a + b;
        const double fd = ((D(MF(0x0057b104)) - D(MF(0x0057b100))) * D(MF(M_SETUP))) + D(MF(0x0057b100));
        fr.ratio = UI_GU32(g[k].ratio);
        fr.speed = (float)((((ab / fd) * D(UI_GF(loc + 0x1c))) * D(MF(0x0057b084))) * KF(0x3dd67750u));
        if (!(fabs(D(MF(g[k].ratio))) > KF(K_EPS))) fr.ratio = UI_GU32(g[k].setup);
        const int32_t mph = x87_ftol((D(fr.speed) / D(*(volatile float*)&fr.ratio)) + KF(0x3f000000u));
        UI_sprintf(fr.buf, (const char*)0x004e59b0, mph);                              // "%d"
        locale(fr.buf);
        style_draw(0x11, self->x + g[k].dx, self->y + g[k].dy, fr.buf);
    }
}
static void fp_DrivetrainControl_Draw(Footprint& f, DrivetrainControl*, Edx, gxCanvas* c) {
    fp_draw_base(f, c, 0, 0);
    const int32_t n = UI_G32(M_CARFILE_108) - 1;
    if (n > 0) f.add((void*)(uintptr_t)0x0057afb0, (uint32_t)(n < 0x40000 ? n : 0x40000) * 4u, "the setup's gear ratios");
}
PORT_FN(0x004976d0, "DrivetrainControl::Draw", DrivetrainControl_Draw_n, fp_DrivetrainControl_Draw)

// the widgets: a picture, the torque curve (its own control), then per gear a label, its ratio and arrows (greyed out
// when the car file fixes the gears: its first ratio isn't 0), the sixth gear in its own group, and the final drive
static void __fastcall DrivetrainControl_Added_n(DrivetrainControl* self, Edx) {
    struct {
        volatile float t;                                // F+0x10
        UIDialogItem items[33];                          // F+0x14
    } fr;
    UIDialogItem* it = fr.items;
    xl_once(M_ONCE_DRIVE_ADDED, 1, 0x005d59f0, 0x004fa098, 0x00498a30);
    xl_once(M_ONCE_DRIVE_ADDED, 2, 0x005d59e0, 0x004fa078, 0x00498a20);
    it_ctor(&it[0], 0xe, 0, 0x23, 0xec, 0, 0, 0x004f9ecc, 0, 0, 0, 0, 0, 0, 0);
    it_ctor(&it[1], 0x17, 0, 0xd6, 0xa9, 0xbf, 0x4b, T_EMPTY, 0, P(&self->torque), 0, 0, 0, 0, 0);
    // bl: the car file's first ratio isn't above epsilon (the setup's gears are the ones edited)
    auto fixed = []() { return !(fabs(D(MF(0x0057b120))) > KF(K_EPS)); };
    static const uint32_t labels[6] = {0x004fa074, 0x004fa068, 0x004fa064, 0x004fa060, 0x004fa05c, 0x004fa058};   // I .. VI
    UIDialogItem* p = &it[2];
    for (int32_t gear = 1; gear <= 6; gear++) {
        const uint32_t y = 0x8c + 0x14u * (uint32_t)(gear - 1);
        it_ctor(p++, 5, 0, 0x46, y, 0, 0, labels[gear - 1], 0, 0, gear == 3 ? lstyle_r() : lstyle(), 0, 0, 0, 0);
        {
            const bool bl = fixed();
            const uint32_t var = P(ccall<float*>(F_get_gear, gear));
            it_ctor(p++, 6, 0, 0x7d, y, 0, 0, 0x004fa06c, 0, var, bl ? 0x15 : 0x16, K_1, 0, 0, 0);          // "%4.2f"
        }
        {
            const bool bl = fixed();
            const uint32_t var = P(ccall<float*>(F_get_gear, gear));
            if (gear >= 5) it_set(p++, 0x10, 0, 0x69, y, 0, 0, T_EMPTY, bl ? 0xc4 : 0, var, 0, 0x3dcccccdu, 0x40800000u, 0, 0);
            else it_ctor(p++, 0x10, 0, 0x69, y, 0, 0, T_EMPTY, bl ? 0xc4 : 0, var, 0, 0x3dcccccdu, 0x40800000u, 0, 0);
        }
        {
            const bool bl = fixed();
            const uint32_t var = P(ccall<float*>(F_get_gear, gear));
            it_ctor(p++, 0x10, 0, 0xac, y, 0, 0, T_EMPTY, bl ? 0xc4 : 0, var, 1, 0x3dcccccdu, 0x40800000u, 0, 0);
        }
        if (gear == 5) it_ctor(p++, 1, 0, 0, 0, 0, 0, 0, 0, P(&self->group6), 0, 0, 0, 0, 0);     // the sixth gear's group
    }
    it_ctor(p++, 1, 1, 0, 0, 0, 0, 0, 0, P(&self->group6), 0, 0, 0, 0, 0);
    // the final drive (0x57af60, between the car file's 0x57b100 and 0x57b104)
    it_ctor(p++, 5, 0, 0xd2, 0x8c, 0, 0, P(xl_text(0x005d59f0)), 0, 0, lstyle_r(), 0, 0, 0, 0);
    {
        const bool n = near_reg(absdiff(0x0057b100, 0x0057b104));
        const float scale = (float)(D(MF(0x0057b104)) - D(MF(0x0057b100)));
        const uint32_t off = UI_GU32(0x0057b100);
        it_ctor(p++, 6, 0, 0x159, 0x8c, 0, 0, 0x004fa050, 0, M_SETUP, n ? 0x16 : 0x15, f_bits(scale), off, 0, 0);   // "%1.2f"
    }
    {
        const double d = absdiff(0x0057b100, 0x0057b104);
        fr.t = (float)d;
        it_set(p++, 0x10, 0, 0x145, 0x8c, 0, 0, T_EMPTY, near_reg(d) ? 0 : 0x15, M_SETUP, 0, 0, K_1, 0, 0);
        it_ctor(p++, 0x10, 0, 0x188, 0x8c, 0, 0, T_EMPTY, near_bits(d) ? 0 : 0x15, M_SETUP, 1, 0, K_1, 0, 0);
    }
    it_end(p);
    add_items(self, fr.items);
}
PORT_FN(0x00497e10, "DrivetrainControl::Added", DrivetrainControl_Added_n, fp_added<DrivetrainControl>)

// =====================================================================================================================
// AeroControl: the spoilers (front / rear numbers with arrows), the downforce bars, the aero kit
// =====================================================================================================================
static void __fastcall AeroControl_Create_n(AeroControl* self, Edx) { notify(self, 0, M_SETUP, 0x8c); }
PORT_FN(0x00498d20, "AeroControl::Create", AeroControl_Create_n, fp_create<AeroControl>)

static void __fastcall AeroControl_Callback_n(AeroControl* self, Edx, int32_t, const void*) { dirty(self); }
PORT_FN(0x00498d40, "AeroControl::Callback", AeroControl_Callback_n, fp_callback<AeroControl>)

// the kit's groups (an adjustable kit's shown when one is fitted and the car has adjustable wings), the downforce and
// drag bars and their figures
static void __fastcall AeroControl_Draw_n(AeroControl* self, Edx, gxCanvas* canvas) {
    struct {
        volatile float df_f, df_r, drag;                 // F+0xc, F+0x10, F+0x14
        char buf[0x50];                                  // F+0x18
    } fr;
    const bool cl = fabs(D(MF(0x0057b254))) > KF(K_EPS);
    const bool al = fabs(D(MF(0x0057b25c))) > KF(K_EPS);
    if (UI_G32(0x0057afd0) != 0 && (cl || al)) {
        ccall<void>(F_UIHideGroup, (uint32_t)self->group_fixed);
        ccall<void>(F_UIShowGroup, (uint32_t)self->group_adjust);
    } else {
        ccall<void>(F_UIShowGroup, (uint32_t)self->group_fixed);
        ccall<void>(F_UIHideGroup, (uint32_t)self->group_adjust);
    }
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp[0], self->x, self->y, 0);
    const int32_t edi = self->x + 0xa;
    int32_t ebx = self->y + 0xa;
    stamp_at(self->stamp[1], edi, ebx, 0);
    fr.df_f = (float)(-D(MF(0x0057b4cc)));
    fr.df_r = (float)(-D(MF(0x0057b4d0)));
    const double s = (D(MF(0x0057b4d8)) + D(MF(0x0057b4d4))) + D(MF(0x0057b224));
    fr.drag = (float)s;
    {
        double v = KF(K_1) - s;
        v = !(0.0 >= v) ? v : 0.0;
        const uint32_t c = UI_GU32(M_C_57A780);
        const int32_t t = x87_ftol((v * KF(0x42b40000u)) + KF(0x3f000000u));
        rect(edi, ebx + 0x56, add32(t, edi), ebx + 0x64, c);
    }
    {
        double v = KF(K_1) - D(fr.df_f);
        v = !(0.0 >= v) ? v : 0.0;
        const uint32_t c = UI_GU32(M_C_57A780);
        const int32_t t = x87_ftol((v * KF(0x41c80000u)) + KF(0x3f000000u));
        rect(edi + 0x2c, ebx, edi + 0x3b, add32(t, ebx), c);
    }
    {
        double v = KF(K_1) - D(fr.df_r);
        v = !(0.0 >= v) ? v : 0.0;
        const uint32_t c = UI_GU32(M_C_57A780);
        const int32_t t = x87_ftol((v * KF(0x41c80000u)) + KF(0x3f000000u));
        rect(edi + 0x94, ebx, edi + 0xa2, add32(t, ebx), c);
    }
    UI_sprintf(fr.buf, (const char*)0x004fa0b8, D(fr.df_f));                           // "%0.2f"
    style_draw(0x14, edi + 0x28, ebx + 0xa, fr.buf);
    UI_sprintf(fr.buf, (const char*)0x004fa0b8, D(fr.df_r));
    style_draw(0x14, edi + 0x90, ebx + 0xa, fr.buf);
    UI_sprintf(fr.buf, (const char*)0x004fa0b8, D(fr.drag));
    ebx += 0x5a;
    style_draw(0x14, edi + 0x96, ebx, fr.buf);
}
static void fp_AeroControl_Draw(Footprint& f, AeroControl*, Edx, gxCanvas* c) {
    fp_draw_base(f, c, 0, 0);
    ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
}
PORT_FN(0x00498aa0, "AeroControl::Draw", AeroControl_Draw_n, fp_AeroControl_Draw)

// the widgets: a picture, two headings, the spoilers' numbers and arrows in the fixed kit's group; then, for a car with
// adjustable wings, the adjustable kit's group again (its numbers without greying) and the kit's three choices
static void __fastcall AeroControl_Added_n(AeroControl* self, Edx) {
    struct {
        volatile uint8_t b11, b12, b13;                  // F+0x11..0x13
        volatile float t;                                // F+0x14
        UIDialogItem b[13];                              // F+0x18 (the second list; its first dword also the float at +0x18)
        UIDialogItem a[13];                              // F+0x2f0
    } fr;
    volatile float* t18 = (volatile float*)&fr.b[0];                                   // [esp+0x18]
    xl_once(M_ONCE_AERO_ADDED, 1, 0x005d5960, 0x004f9f44, 0x00499a00);
    xl_once(M_ONCE_AERO_ADDED, 2, 0x005d5970, 0x004f9f38, 0x004999f0);
    xl_once(M_ONCE_AERO_ADDED, 4, 0x005d5980, 0x004fa130, 0x004999e0);
    UIDialogItem* it = fr.a;
    it_ctor(&it[0], 0xe, 0, 0x23, 0xec, 0, 0, 0x004f9ecc, 0, 0, 0, 0, 0, 0, 0);
    it_ctor(&it[1], 5, 0, 0xd9, 0x8c, 0, 0, P(xl_text(0x005d5960)), 0, 0, 0x12, 0, 0, 0, 0);
    it_ctor(&it[2], 5, 0, 0x13d, 0x8c, 0, 0, P(xl_text(0x005d5970)), 0, 0, 0x12, 0, 0, 0, 0);
    it_set(&it[3], 5, 0, 0x46, 0x9b, 0, 0, P(xl_text(0x005d5980)), 0, 0, lstyle(), 0, 0, 0, 0);
    it_ctor(&it[4], 1, 0, 0, 0, 0, 0, 0, 0, P(&self->group_fixed), 0, 0, 0, 0, 0);
    {
        const double d = absdiff(0x0057b238, 0x0057b240);
        *t18 = (float)d;
        const bool a = near_reg(d), c = near_bits(d);
        it_ctor(&it[5], 6, 0, 0xd2, 0x9b, 0, 0, c ? T_DASH : T_F10, 0, 0x0057afa8, a ? 0x16 : 0x15, K_100, 0, 0, 0);
    }
    {
        const double d = absdiff(0x0057b238, 0x0057b240);
        *t18 = (float)d;
        it_set(&it[6], 0x10, 0, 0xbe, 0x9b, 0, 0, T_EMPTY, near_reg(d) ? 0 : 0x15, 0x0057afa8, 0, 0, K_1, 0, 0);
        it_ctor(&it[7], 0x10, 0, 0x101, 0x9b, 0, 0, T_EMPTY, near_bits(d) ? 0 : 0x15, 0x0057afa8, 1, 0, K_1, 0, 0);
    }
    {
        const double d = absdiff(0x0057b23c, 0x0057b244);
        *t18 = (float)d;
        const bool a = near_reg(d), c = near_bits(d);
        it_set(&it[8], 6, 0, 0x136, 0x9b, 0, 0, c ? T_DASH : T_F10, 0, 0x0057afac, a ? 0x16 : 0x15, K_100, 0, 0, 0);
        it_set(&it[9], 0x10, 0, 0x122, 0x9b, 0, 0, T_EMPTY, c ? 0 : 0x15, 0x0057afac, 0, 0, K_1, 0, 0);
        it_ctor(&it[10], 0x10, 0, 0x165, 0x9b, 0, 0, T_EMPTY, c ? 0 : 0x15, 0x0057afac, 1, 0, K_1, 0, 0);
    }
    it_ctor(&it[11], 1, 1, 0, 0, 0, 0, 0, 0, P(&self->group_fixed), 0, 0, 0, 0, 0);
    it_end(&it[12]);
    add_items(self, fr.a);
    // the adjustable kit
    {
        const double d = fabs(D(MF(0x0057b254)));
        *t18 = (float)d;
        const bool adj = d > KF(K_EPS) || fabs(D(MF(0x0057b25c))) > KF(K_EPS);
        if (!adj) {
            it_ctor(&fr.b[0], 1, 0, 0, 0, 0, 0, 0, 0, P(&self->group_adjust), 0, 0, 0, 0, 0);
            it_ctor(&fr.b[1], 1, 1, 0, 0, 0, 0, 0, 0, P(&self->group_adjust), 0, 0, 0, 0, 0);
            it_end(&fr.b[2]);
            add_items(self, fr.b);
            return;
        }
    }
    fr.b11 = 0;
    if (fabs(D(MF(0x0057b23c)) - D(MF(0x0057b244))) > KF(K_EPS)) fr.b11 = 1;
    fr.b12 = 0;
    if (*(volatile int32_t*)t18 > (int32_t)K_EPS) fr.b12 = 1;
    fr.b13 = 0;
    if (fabs(D(MF(0x0057b25c))) > KF(K_EPS)) fr.b13 = 1;
    xl_once(M_ONCE_AERO_ADDED, 0x08, 0x005d5990, 0x004fa120, 0x004999d0);
    xl_once(M_ONCE_AERO_ADDED, 0x10, 0x005d59a0, 0x004fa108, 0x004999c0);
    xl_once(M_ONCE_AERO_ADDED, 0x20, 0x005d59b0, 0x004fa0f4, 0x004999b0);
    xl_once(M_ONCE_AERO_ADDED, 0x40, 0x005d59c0, 0x004fa0dc, 0x004999a0);
    xl_once(M_ONCE_AERO_ADDED, 0x80, 0x005d59d0, 0x004fa0c0, 0x00499990);
    it = fr.b;
    it_ctor(&it[0], 1, 0, 0, 0, 0, 0, 0, 0, P(&self->group_adjust), 0, 0, 0, 0, 0);
    it_ctor(&it[1], 6, 0, 0xd2, 0x9b, 0, 0, near_reg(absdiff(0x0057b238, 0x0057b240)) ? T_DASH : T_F10, 0, 0x0057afa8, 0x16, K_100, 0, 0, 0);
    fr.t = (float)absdiff(0x0057b238, 0x0057b240);
    it_set(&it[2], 0x10, 0, 0xbe, 0x9b, 0, 0, T_EMPTY, 0, 0x0057afa8, 0, 0, K_1, 0, 0);
    it_ctor(&it[3], 0x10, 0, 0x101, 0x9b, 0, 0, T_EMPTY, 0, 0x0057afa8, 1, 0, K_1, 0, 0);
    {
        const double d = absdiff(0x0057b23c, 0x0057b244);
        fr.t = (float)d;
        it_set(&it[4], 6, 0, 0x136, 0x9b, 0, 0, near_reg(d) ? T_DASH : T_F10, 0, 0x0057afac, 0x16, K_100, 0, 0, 0);
    }
    it_set(&it[5], 0x10, 0, 0x122, 0x9b, 0, 0, T_EMPTY, 0, 0x0057afac, 0, 0, K_1, 0, 0);
    it_ctor(&it[6], 0x10, 0, 0x165, 0x9b, 0, 0, T_EMPTY, 0, 0x0057afac, 1, 0, K_1, 0, 0);
    it_ctor(&it[7], 1, 1, 0, 0, 0, 0, 0, 0, P(&self->group_adjust), 0, 0, 0, 0, 0);
    // the kit: none / low drag / high downforce (radio buttons on 0x57afd0), the last two only where the car has them
    it_ctor(&it[8], 5, 0, 0x46, 0xaf, 0, 0, P(xl_text(0x005d5990)), 0, 0, lstyle(), 0, 0, 0, 0);
    {
        const char* t = fr.b11 != 0 ? xl_text(0x005d59a0) : xl_text(0x005d59b0);
        it_ctor(&it[9], 0xc, 0, 0xbe, 0xaf, 0, 0, P(t), 0, 0x0057afd0, 8, 0, 0, 0, 0);
    }
    {
        const uint32_t var = fr.b12 != 0 ? 0x0057afd0u : 0u;
        const char* t = xl_text(0x005d59c0);
        it_set(&it[10], 0xc, 0, 0xbe, 0xbe, 0, 0, P(t), 1, var, 8, 0, 0, 0, 0);
    }
    {
        const uint32_t var = fr.b13 != 0 ? 0x0057afd0u : 0u;
        it_ctor(&it[11], 0xc, 0, 0xbe, 0xcd, 0, 0, P(xl_text(0x005d59d0)), 2, var, 8, 0, 0, 0, 0);
    }
    it_end(&it[12]);
    add_items(self, fr.b);
}
PORT_FN(0x00498d50, "AeroControl::Added", AeroControl_Added_n, fp_added<AeroControl>)

// =====================================================================================================================
// FileControl: the setup's summary (every figure the other controls edit) and Load / Save / Default
// =====================================================================================================================
static void __fastcall FileControl_Create_n(FileControl* self, Edx) {
    notify(self, 0, M_SETUP, 0x8c);
    notify(self, 1, M_SETUP_NAME, 0x40);
}
PORT_FN(0x0049a430, "FileControl::Create", FileControl_Create_n, fp_create<FileControl>)

static void __fastcall FileControl_Callback_n(FileControl* self, Edx, int32_t, const void*) { dirty(self); }
PORT_FN(0x0049a460, "FileControl::Callback", FileControl_Callback_n, fp_callback<FileControl>)

// FIX CANDIDATE: the gear ratios are sprintf'd ("%1.2f") into 32 bytes at the frame's top: a ratio of 1e28 or more
// overruns it (into the saved registers and the return address).
static void __fastcall FileControl_Draw_n(FileControl* self, Edx, gxCanvas* canvas) {
    struct {
        const char* volatile gears[6];                   // F+0x10
        char buf[0x100];                                 // F+0x28
        char num[0x20];                                  // F+0x128
    } fr;
    xl_once(M_ONCE_FILE_DRAW, 1, 0x005d5940, 0x004fa098, 0x0049a6e0);
    xl_once(M_ONCE_FILE_DRAW, 2, 0x005d5950, 0x004fa160, 0x0049a6d0);
    xl_once(M_ONCE_FILE_DRAW, 4, 0x005d5930, 0x004fa14c, 0x0049a6c0);
    ccall<void*>(F_gxSetCanvas, canvas);
    stamp_at(self->stamp, self->x + 0x82, self->y + 5, 0);
    {
        const char* name = UI_G8(M_SETUP_NAME) != 0 ? (const char*)(uintptr_t)M_SETUP_NAME : xl_text(0x0057b758);
        style_draw(0xc, self->x + 0xa, self->y + 0xa, name);
    }
    int32_t edi = self->y + 0x37;
    int32_t ebx = self->x + 0x14;
    style_draw(0xb, ebx - 0xa, self->y + 0x23, xl_text(0x005d5930));
    const int32_t ebp = ebx + 0xa5;
    auto label = [&](uint32_t xl) {
        UI_sprintf(fr.buf, (const char*)0x004fa148, xl_text(xl));                     // "%s:"
        style_draw(0xc, ebx, edi, fr.buf);
    };
    auto pct = [&](uint32_t v, int32_t x) {
        UI_sprintf(fr.buf, (const char*)0x004e59b0, x87_ftol((D(MF(v)) * KF(K_100)) + KF(0x3f000000u)));   // "%d"
        style_draw(0xd, x, edi, fr.buf);
    };
    auto units = [&](uint32_t hi, uint32_t lo, uint32_t s, int32_t x) {
        UI_sprintf(fr.buf, (const char*)0x004f9fa4, lerp(hi, lo, (const volatile float*)(uintptr_t)s));       // "%1.1f"
        locale(fr.buf);
        style_draw(0xd, x, edi, fr.buf);
    };
    label(0x0057b288); pct(0x0057af70, ebp); pct(0x0057af78, ebx + 0xdc); edi += 0xc;
    label(0x0057a760); pct(0x0057af80, ebp); pct(0x0057af88, ebx + 0xdc); edi += 0xc;
    label(0x0057b670); pct(0x0057af98, ebp); pct(0x0057af9c, ebx + 0xdc); edi += 0xc;
    label(0x0057b528); pct(0x0057af90, ebp); pct(0x0057af94, ebx + 0xdc); edi += 0xc;
    label(0x0057b6b0); units(0x0057b19c, 0x0057b16c, 0x0057afe0, ebp); units(0x0057b1a0, 0x0057b170, 0x0057afe4, ebx + 0xdc); edi += 0xc;
    label(0x0057b768); units(0x0057b194, 0x0057b164, 0x0057afd8, ebp); units(0x0057b198, 0x0057b168, 0x0057afdc, ebx + 0xdc); edi += 0x18;
    label(0x0057a798);
    UI_sprintf(fr.buf, (const char*)0x004e59b0, x87_ftol(((KF(K_1) - D(MF(0x0057af68))) * KF(K_100)) + KF(0x3f000000u)));
    style_draw(0xd, ebp, edi, fr.buf);
    pct(0x0057af68, ebx + 0xdc);
    edi += 0xc;
    label(0x0057aea0);
    UI_sprintf(fr.buf, (const char*)0x004e59b0, x87_ftol((D(MF(0x0057afe8)) * D(MF(0x0057b1d4))) + KF(0x3f000000u)));
    style_draw(0xd, ebp, edi, fr.buf);
    edi += 0x18;
    label(0x0057a788); pct(0x0057afa8, ebp); pct(0x0057afac, ebx + 0xdc); edi += 0xc;
    label(0x005d5940);
    UI_sprintf(fr.buf, (const char*)0x004fa050, lerp(0x0057b104, 0x0057b100, (const volatile float*)(uintptr_t)M_SETUP));   // "%1.2f"
    locale(fr.buf);
    ebx += 0xc0;
    style_draw(0xd, ebx, edi, fr.buf);
    // the gear box: I .. VI and their ratios
    ebx = self->x + 0x12c;
    edi = self->y + 0x37;
    style_draw(0xb, ebx - 0xa, self->y + 0x23, xl_text(0x005d5950));
    fr.gears[0] = (const char*)0x004fa074;
    fr.gears[1] = (const char*)0x004fa068;
    fr.gears[2] = (const char*)0x004fa064;
    const int32_t esi = ebx + 0x64;
    fr.gears[3] = (const char*)0x004fa060;
    fr.gears[4] = (const char*)0x004fa05c;
    fr.gears[5] = (const char*)0x004fa058;
    for (uint32_t o = 0; o < 0x18;) {
        const char* g = fr.gears[o / 4];
        o += 4;
        style_draw(0xc, ebx, edi, g);
        UI_sprintf(fr.num, (const char*)0x004fa050, D(UI_GF(0x0057afacu + o)));     // FIX CANDIDATE: a ratio >= 1e28 overruns num
        locale(fr.num);
        style_draw(0xd, esi, edi, fr.num);
        edi += 0xc;
    }
}
static void fp_FileControl_Draw(Footprint& f, FileControl*, Edx, gxCanvas* c) {
    if (!fp_draw_base(f, c, M_ONCE_FILE_DRAW, 7)) return;
    for (uint32_t xl : {0x005d5940u, 0x005d5950u, 0x005d5930u, 0x0057b758u, 0x0057b288u, 0x0057a760u, 0x0057b670u, 0x0057b528u,
                        0x0057b6b0u, 0x0057b768u, 0x0057a798u, 0x0057aea0u, 0x0057a788u})
        fp_xl(f, xl);
}
PORT_FN(0x00499a50, "FileControl::Draw", FileControl_Draw_n, fp_FileControl_Draw)

// the three buttons: Save (save_setup_cb), Load (load_setup_cb), Default (default_setup_cb)
static void __fastcall FileControl_Added_n(FileControl* self, Edx) {
    UIDialogItem items[4];                               // F+0xc
    xl_once(M_ONCE_FILE_ADDED, 1, 0x005d5900, 0x004fa1a4, 0x0049a6b0);
    xl_once(M_ONCE_FILE_ADDED, 2, 0x005d5910, 0x004fa190, 0x0049a6a0);
    xl_once(M_ONCE_FILE_ADDED, 4, 0x005d5920, 0x004fa17c, 0x0049a690);
    it_set(&items[0], 2, 0, 0x145, 0x1a9, 0, 0, P(xl_text(0x005d5900)), 0, F_save_setup_cb, 0, 0, 0, 0, 0);
    it_set(&items[1], 2, 0, 0x1a9, 0x1a9, 0, 0, P(xl_text(0x005d5910)), 0, F_load_setup_cb, 0, 0, 0, 0, 0);
    it_set(&items[2], 2, 0, 0x20d, 0x1a9, 0, 0, P(xl_text(0x005d5920)), 0, F_default_setup_cb, 0, 0, 0, 0, 0);
    it_end(&items[3]);
    add_items(self, items);
}
PORT_FN(0x0049a470, "FileControl::Added", FileControl_Added_n, fp_added<FileControl>)

// =====================================================================================================================
// the deleting destructors: the stamps forgotten (under the class's own vtable), the base's vtable, freed if asked
// =====================================================================================================================
static __forceinline void* finish_dtor(McCtl* self, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
static void* __fastcall BalanceControl_ddtor_n(BalanceControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp[0];
    self->vtbl = (const void*)(uintptr_t)VT_BalanceControl;
    ccall<void>(F_gxForgetStamp, s0);
    ccall<void>(F_gxForgetStamp, (void*)self->stamp[1]);
    return finish_dtor(self, flags);
}
PORT_FN(0x00494340, "BalanceControl::scalar deleting destructor", BalanceControl_ddtor_n, fp_dtor<BalanceControl>)

static void* __fastcall ChassisControl_ddtor_n(ChassisControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp[0];
    self->vtbl = (const void*)(uintptr_t)VT_ChassisControl;
    ccall<void>(F_gxForgetStamp, s0);
    ccall<void>(F_gxForgetStamp, (void*)self->stamp[1]);
    ccall<void>(F_gxForgetStamp, (void*)self->stamp[2]);
    return finish_dtor(self, flags);
}
PORT_FN(0x00495a50, "ChassisControl::vector deleting destructor", ChassisControl_ddtor_n, fp_dtor<ChassisControl>)

static void* __fastcall AlignControl_ddtor_n(AlignControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp[0];
    self->vtbl = (const void*)(uintptr_t)VT_AlignControl;
    ccall<void>(F_gxForgetStamp, s0);
    for (int i = 1; i < 6; i++) ccall<void>(F_gxForgetStamp, (void*)self->stamp[i]);
    return finish_dtor(self, flags);
}
PORT_FN(0x00497440, "AlignControl::vector deleting destructor", AlignControl_ddtor_n, fp_dtor<AlignControl>)

static void* __fastcall TorqueCurveControl_ddtor_n(TorqueCurveControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp;
    self->vtbl = (const void*)(uintptr_t)VT_TorqueCurveControl;
    ccall<void>(F_gxForgetStamp, s0);
    return finish_dtor(self, flags);
}
PORT_FN(0x00497690, "TorqueCurveControl::scalar deleting destructor", TorqueCurveControl_ddtor_n, fp_dtor<TorqueCurveControl>)

static void* __fastcall DrivetrainControl_ddtor_n(DrivetrainControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp[0];
    self->vtbl = (const void*)(uintptr_t)VT_DrivetrainControl;
    ccall<void>(F_gxForgetStamp, s0);
    ccall<void>(F_gxForgetStamp, (void*)self->stamp[1]);
    ccall<void>(F_TireEnd);
    self->torque.vtbl = (const void*)(uintptr_t)VT_TorqueCurveControl;
    ccall<void>(F_gxForgetStamp, (void*)self->torque.stamp);
    self->vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    self->torque.vtbl = (const void*)(uintptr_t)VT_UICustomControl;
    if (flags & 1) ccall<void>(F_Delete, (void*)self);
    return self;
}
PORT_FN(0x00498a40, "DrivetrainControl::scalar deleting destructor", DrivetrainControl_ddtor_n, fp_dtor<DrivetrainControl>)

static void* __fastcall AeroControl_ddtor_n(AeroControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp[0];
    self->vtbl = (const void*)(uintptr_t)VT_AeroControl;
    ccall<void>(F_gxForgetStamp, s0);
    ccall<void>(F_gxForgetStamp, (void*)self->stamp[1]);
    return finish_dtor(self, flags);
}
PORT_FN(0x00499a10, "AeroControl::vector deleting destructor", AeroControl_ddtor_n, fp_dtor<AeroControl>)

static void* __fastcall FileControl_ddtor_n(FileControl* self, Edx, uint32_t flags) {
    void* s0 = self->stamp;
    self->vtbl = (const void*)(uintptr_t)VT_FileControl;
    ccall<void>(F_gxForgetStamp, s0);
    return finish_dtor(self, flags);
}
PORT_FN(0x0049a6f0, "FileControl::vector deleting destructor", FileControl_ddtor_n, fp_dtor<FileControl>)

// =====================================================================================================================
// the function-local Xlators' destructor helpers: each a bare `ret` (the $E atexit functions destroy the Xlators)
// =====================================================================================================================
static void fp_pure0(Footprint& f) { f.pure = true; }
static void __cdecl xlh_00() {}
PORT_FN(0x00494300, "BalanceControl::Draw::XL_Total (destructor helper)", xlh_00, fp_pure0)
static void __cdecl xlh_01() {}
PORT_FN(0x00494310, "BalanceControl::Draw::XL_Aero (destructor helper)", xlh_01, fp_pure0)
static void __cdecl xlh_02() {}
PORT_FN(0x00494320, "BalanceControl::Draw::XL_Align (destructor helper)", xlh_02, fp_pure0)
static void __cdecl xlh_03() {}
PORT_FN(0x00494330, "BalanceControl::Draw::XL_Shocks (destructor helper)", xlh_03, fp_pure0)
static void __cdecl xlh_04() {}
PORT_FN(0x00495970, "ChassisControl::Added::XL_AntiRoll (destructor helper)", xlh_04, fp_pure0)
static void __cdecl xlh_05() {}
PORT_FN(0x00495980, "ChassisControl::Added::XL_Springs (destructor helper)", xlh_05, fp_pure0)
static void __cdecl xlh_06() {}
PORT_FN(0x00495990, "ChassisControl::Added::XL_Rebound (destructor helper)", xlh_06, fp_pure0)
static void __cdecl xlh_07() {}
PORT_FN(0x004959a0, "ChassisControl::Added::XL_Bump (destructor helper)", xlh_07, fp_pure0)
static void __cdecl xlh_08() {}
PORT_FN(0x004959b0, "ChassisControl::Added::XL_Rear (destructor helper)", xlh_08, fp_pure0)
static void __cdecl xlh_09() {}
PORT_FN(0x004959c0, "ChassisControl::Added::XL_Front (destructor helper)", xlh_09, fp_pure0)
static void __cdecl xlh_10() {}
PORT_FN(0x004959d0, "ChassisControl::Draw::XL_FrontAntiRoll (destructor helper)", xlh_10, fp_pure0)
static void __cdecl xlh_11() {}
PORT_FN(0x004959e0, "ChassisControl::Draw::XL_RearAntiRoll (destructor helper)", xlh_11, fp_pure0)
static void __cdecl xlh_12() {}
PORT_FN(0x004959f0, "ChassisControl::Draw::XL_RearSprings (destructor helper)", xlh_12, fp_pure0)
static void __cdecl xlh_13() {}
PORT_FN(0x00495a00, "ChassisControl::Draw::XL_FrontSprings (destructor helper)", xlh_13, fp_pure0)
static void __cdecl xlh_14() {}
PORT_FN(0x00495a10, "ChassisControl::Draw::XL_RearShocks (destructor helper)", xlh_14, fp_pure0)
static void __cdecl xlh_15() {}
PORT_FN(0x00495a20, "ChassisControl::Draw::XL_FrontShocks (destructor helper)", xlh_15, fp_pure0)
static void __cdecl xlh_16() {}
PORT_FN(0x00495a30, "ChassisControl::Draw::XL_Rebound (destructor helper)", xlh_16, fp_pure0)
static void __cdecl xlh_17() {}
PORT_FN(0x00495a40, "ChassisControl::Draw::XL_Bump (destructor helper)", xlh_17, fp_pure0)
static void __cdecl xlh_18() {}
PORT_FN(0x004973b0, "AlignControl::Added::XL_WheelLock (destructor helper)", xlh_18, fp_pure0)
static void __cdecl xlh_19() {}
PORT_FN(0x004973c0, "AlignControl::Added::XL_BrakeBias (destructor helper)", xlh_19, fp_pure0)
static void __cdecl xlh_20() {}
PORT_FN(0x004973d0, "AlignControl::Added::XL_Height (destructor helper)", xlh_20, fp_pure0)
static void __cdecl xlh_21() {}
PORT_FN(0x004973e0, "AlignControl::Added::XL_Camber (destructor helper)", xlh_21, fp_pure0)
static void __cdecl xlh_22() {}
PORT_FN(0x004973f0, "AlignControl::Added::XL_ToeIn (destructor helper)", xlh_22, fp_pure0)
static void __cdecl xlh_23() {}
PORT_FN(0x00497400, "AlignControl::Added::XL_Rear (destructor helper)", xlh_23, fp_pure0)
static void __cdecl xlh_24() {}
PORT_FN(0x00497410, "AlignControl::Added::XL_Front (destructor helper)", xlh_24, fp_pure0)
static void __cdecl xlh_25() {}
PORT_FN(0x00497420, "AlignControl::Draw::XL_WheelLock (destructor helper)", xlh_25, fp_pure0)
static void __cdecl xlh_26() {}
PORT_FN(0x00497430, "AlignControl::Draw::XL_BrakeBias (destructor helper)", xlh_26, fp_pure0)
static void __cdecl xlh_27() {}
PORT_FN(0x00498a20, "DrivetrainControl::Added::XL_EnginePower (destructor helper)", xlh_27, fp_pure0)
static void __cdecl xlh_28() {}
PORT_FN(0x00498a30, "DrivetrainControl::Added::XL_FinalDrive (destructor helper)", xlh_28, fp_pure0)
static void __cdecl xlh_29() {}
PORT_FN(0x00499990, "AeroControl::Added::XL_HighDownforce (destructor helper)", xlh_29, fp_pure0)
static void __cdecl xlh_30() {}
PORT_FN(0x004999a0, "AeroControl::Added::XL_LowDrag (destructor helper)", xlh_30, fp_pure0)
static void __cdecl xlh_31() {}
PORT_FN(0x004999b0, "AeroControl::Added::XL_None (destructor helper)", xlh_31, fp_pure0)
static void __cdecl xlh_32() {}
PORT_FN(0x004999c0, "AeroControl::Added::XL_Adjustable (destructor helper)", xlh_32, fp_pure0)
static void __cdecl xlh_33() {}
PORT_FN(0x004999d0, "AeroControl::Added::XL_AeroKit (destructor helper)", xlh_33, fp_pure0)
static void __cdecl xlh_34() {}
PORT_FN(0x004999e0, "AeroControl::Added::XL_SpoilerSize (destructor helper)", xlh_34, fp_pure0)
static void __cdecl xlh_35() {}
PORT_FN(0x004999f0, "AeroControl::Added::XL_Rear (destructor helper)", xlh_35, fp_pure0)
static void __cdecl xlh_36() {}
PORT_FN(0x00499a00, "AeroControl::Added::XL_Front (destructor helper)", xlh_36, fp_pure0)
static void __cdecl xlh_37() {}
PORT_FN(0x0049a690, "FileControl::Added::XL_Default (destructor helper)", xlh_37, fp_pure0)
static void __cdecl xlh_38() {}
PORT_FN(0x0049a6a0, "FileControl::Added::XL_Load (destructor helper)", xlh_38, fp_pure0)
static void __cdecl xlh_39() {}
PORT_FN(0x0049a6b0, "FileControl::Added::XL_Save (destructor helper)", xlh_39, fp_pure0)
static void __cdecl xlh_40() {}
PORT_FN(0x0049a6c0, "FileControl::Draw::XL_Summary (destructor helper)", xlh_40, fp_pure0)
static void __cdecl xlh_41() {}
PORT_FN(0x0049a6d0, "FileControl::Draw::XL_GearBox (destructor helper)", xlh_41, fp_pure0)
static void __cdecl xlh_42() {}
PORT_FN(0x0049a6e0, "FileControl::Draw::XL_FinalDrive (destructor helper)", xlh_42, fp_pure0)

#undef D
#undef MF
#undef KF
#undef HTML_LN
}  // namespace menu_car
}  // namespace
