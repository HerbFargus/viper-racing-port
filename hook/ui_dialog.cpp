// ui_dialog.cpp -- M3 UI stage, step U1: ui.obj (all but its widget virtuals, which are in ui_widget.cpp), rewritten
// faithfully (library `ui`).
//
//   UIBegin / UIEnd (the UI's sync object, the four scroll-arrow stamps, the style table: 24 styles described once in
//   a static table, then added; the windows), _UIUpdateTime / UIDeltaT (the frame's time, at most 0.25 s),
//   _UIAddItems (a dialog's item list made into widgets: 23 item types, the buttons' Escape / Return hot keys, the
//   sliders' and scroll bars' arrow buttons), UIDoDialog (a dialog built, run modally and destroyed; its idle function
//   through dialog_idle_func), UIDoMenu, the group calls on the running window, and the stock dialogs: UIDoDratBox,
//   UIDoOkBox, UIDoLongOkBox, UIDoOkCancelBox, UIDoLongOkCancelBox, UIDoYesNoBox, UIDoLongYesNoBox,
//   UIDoYesNoCancelBox, UIDoInputBox (Ctrl-V pastes: inputbox_paste), UIDoOpenFileBox / UIDoSaveFileBox (a list of
//   the matching files: file_idle copies the selection into the name, file_exists); UIStringList; CreateMultiString
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this library's own too).
// The stock dialogs build their item lists on the stack as the original does -- the same items, with every field the
// original stores (UIDialogItem's constructor 0x4091c0 called where the original calls it), the translated button
// texts read after their Xlator is refreshed -- and hand them to UIDoDialog. UIBegin's style descriptions are the
// original's stores, in its order (generated from the disassembly, register by register).
//
// Footprints (main thread). replay_only: every dialog (modal: its own input loop), UIDoMenu, UIDoDialog,
// _UIAddItems, UIBegin / UIEnd (they allocate, load or free), dialog_idle_func (the dialog's idle function: menu
// code), inputbox_paste (the clipboard), file_exists (opens a file), _UIUpdateTime (the wall clock), UIStringList's
// constructor / destructor. The group calls write the running window and its widgets; UIStringList::AddEntry /
// DeleteEntry the list and its entries; file_idle the file boxes' statics and name; CreateMultiString its output.
//
// FIX CANDIDATES (left as the original has them):
//   * UIDoOpenFileBox / UIDoSaveFileBox leave 0x578e78 / 0x578d48 aimed at their own stack frames (the list, the
//     name); file_idle uses them only while the box runs. The name buffer is 300 bytes, the path 260: the sprintf's
//     (directory + pattern, directory + name) have no bound.
//   * UIDoInputBox copies the caller's text into its 256-byte static unbounded (strcpy), and back unbounded.
//   * UIStringList's constructor never initialises `changes` (+0xc), which list boxes watch; AddEntry past the
//     capacity does nothing, silently.
//   * _UIAddItems: a CustomWidget's control gets the item's width and height in its x1 / y1 (+0xc / +0x10), as
//     sizes.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"

namespace {
namespace ui_dialog {
using namespace uit;

#define D(x) ((double)(x))
static __forceinline float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// a variadic function's arguments, registered as 32 dwords (the caller's are in order on the stack: krn_file.cpp's Va)
struct Va {
    uint32_t w[32];
    Va() = default;
    explicit Va(uint32_t) {}
};

// an item as the stock dialogs write it (all 14 dwords)
static __forceinline void item(UIDialogItem* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                               const char* text, int32_t i1c, void* data, int32_t style, int32_t* sel = 0) {
    volatile uint32_t* d = (volatile uint32_t*)it;
    d[0] = (uint32_t)type;
    d[1] = (uint32_t)id;
    d[2] = (uint32_t)x;
    d[3] = (uint32_t)y;
    d[4] = (uint32_t)w;
    d[5] = (uint32_t)h;
    d[6] = (uint32_t)(uintptr_t)text;
    d[7] = (uint32_t)i1c;
    d[8] = (uint32_t)(uintptr_t)data;
    d[9] = (uint32_t)style;
    d[10] = 0;
    d[11] = 0;
    d[12] = (uint32_t)(uintptr_t)sel;
    d[13] = 0;
}
// UIDialogItem::UIDialogItem (replay.obj), by address
static __forceinline void item_ctor(UIDialogItem* it, int32_t type, int32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                                    const char* text, int32_t i1c, void* data, int32_t style, int32_t* sel, const char* s34) {
    tcall<void*>(F_UIDialogItem_ctor, it, type, id, x, y, w, h, text, i1c, data, style, (uint32_t)0, (uint32_t)0, sel, s34);
}
// the end of a list: the static item at 0x578d08, copied (rep movsd)
static __forceinline void item_end(UIDialogItem* it) { crt_copy(it, (const void*)(uintptr_t)S_END_ITEM, 0x38); }
static __forceinline void dialog(UIDialog* d, const char* title, const char* bg, int32_t def, UIDialogItem* items, UIIdle idle) {
    volatile uint32_t* v = (volatile uint32_t*)d;
    v[0] = (uint32_t)(uintptr_t)title;
    v[1] = (uint32_t)(uintptr_t)bg;
    v[2] = (uint32_t)def;
    v[3] = (uint32_t)(uintptr_t)items;
    v[4] = (uint32_t)(uintptr_t)idle;
}
static __forceinline int32_t run_dialog(UIDialog* d, int32_t w, int32_t h) {
    return ccall<int32_t>(F_UIDoDialog, (const UIDialog*)d, w, h, (int32_t)-999, (int32_t)-999, (int32_t)1);
}
// a hot key for a button's text's first letter: tolower((signed char)text[0]), as an unsigned short
static __forceinline int32_t first_key(const char* text) {
    const int c = (int)*(const volatile signed char*)text;
    return (int32_t)(uint16_t)ccall<int>(F_tolower, c);
}
enum : uint32_t { XL_OK = 0x00578ec0, XL_CANCEL = 0x00578cd0, XL_YES = 0x00578e98, XL_NO = 0x00578eb0,
                  XL_DRAT = 0x00578d60, XL_BACK = 0x00578ef8, XL_OVERWRITE_TITLE = 0x00578e88,
                  XL_OVERWRITE_PROMPT = 0x00578d50 };

static void fp_modal2(Footprint& f, const char*, const char*) { f.replay_only = "a dialog: runs modally (its own input loop)"; }
static void fp_modal3(Footprint& f, const char*, const char*, UIIdle) { f.replay_only = "a dialog: runs modally (its own input loop)"; }

// =====================================================================================================================
// UIBegin / UIEnd, the frame time
// =====================================================================================================================
static void __cdecl UIBegin_n() {
    const char* names[4];
    UI_G32(S_UI_SYNC) = ccall<int32_t>(F_SingleBegin, (const char*)0x004f6b78);
    ((const char* volatile*)names)[0] = (const char*)0x004f6b7c;
    ((const char* volatile*)names)[1] = (const char*)0x004f6b88;
    ((const char* volatile*)names)[2] = (const char*)0x004f6b94;
    ((const char* volatile*)names)[3] = (const char*)0x004f6ba0;
    for (int i = 0; i < 4; i++)
        UI_GP(void, S_UI_STAMPS + 4 * i) = ccall<void*>(F_gxGetStamp, ((const char* volatile*)names)[i]);
    ccall<void>(F_UIStyleBegin);
    const uint8_t once = UI_G8(S_UI_BEGUN);
    UI_GU32(S_UI_DT) = 0;
    if (!(once & 1)) {
        // the style descriptions (UIStyleDesc[24] at 0x4f6698), filled once: the original's stores in its order
        // (generated from the disassembly of 0x477a10..0x47834a, register by register: each `mov reg, [addr]` a
        // const read where the original reads it, each store where it stores)
        const uint32_t edx1 = UI_GU32(0x00578d00);
        const uint32_t esi1 = UI_GU32(0x00578ecc);
        UI_G8(0x00578cec) = (uint8_t)(once | 1);
        UI_GU32(0x004f66a4) = edx1;
        const uint32_t ecx1 = UI_GU32(0x00578ea8);
        UI_GU32(0x004f66a8) = esi1;
        UI_GU32(0x004f66ac) = 0x2fu;
        UI_GU32(0x004f66b4) = ecx1;
        UI_GU32(0x004f66b8) = ecx1;
        UI_GU32(0x004f66bc) = esi1;
        UI_GU32(0x004f66a0) = 0xe77c73bcu;
        UI_GU32(0x004f66b0) = 0xbu;
        UI_GU32(0x004f66c4) = 0xcu;
        UI_GU32(0x004f66c8) = 0x4f6bb8u;
        UI_GU32(0x004f66c0) = 0x2fu;
        UI_GU32(0x004f66d8) = edx1;
        UI_GU32(0x004f66dc) = esi1;
        UI_GU32(0x004f66e0) = 0x2fu;
        UI_GU32(0x004f66cc) = 0x4f6bc4u;
        UI_GU32(0x004f66e8) = ecx1;
        UI_GU32(0x004f66ec) = ecx1;
        UI_GU32(0x004f66f0) = esi1;
        UI_GU32(0x004f66f4) = 0x2fu;
        UI_GU32(0x004f66d0) = 0x14u;
        UI_GU32(0x004f66d4) = 0xe77c73bcu;
        UI_GU32(0x004f66e4) = 0xdu;
        UI_GU32(0x004f66f8) = 0xeu;
        UI_GU32(0x004f66fc) = 0x4f6bd0u;
        UI_GU32(0x004f670c) = edx1;
        UI_GU32(0x004f6710) = esi1;
        UI_GU32(0x004f6700) = 0x4f6bdcu;
        UI_GU32(0x004f671c) = ecx1;
        UI_GU32(0x004f6714) = 0x43u;
        UI_GU32(0x004f6720) = ecx1;
        UI_GU32(0x004f6724) = esi1;
        UI_GU32(0x004f6728) = 0x43u;
        UI_GU32(0x004f6704) = 0x14u;
        UI_GU32(0x004f6708) = 0xe77c73bcu;
        UI_GU32(0x004f6718) = 0x13u;
        UI_GU32(0x004f672c) = 0x14u;
        const uint32_t eax1 = UI_GU32(0x00578cf0);
        const uint32_t ebp1 = UI_GU32(0x00578d04);
        UI_GU32(0x004f673c) = eax1;
        UI_GU32(0x004f6740) = eax1;
        UI_GU32(0x004f6730) = 0x4f6be8u;
        UI_GU32(0x004f6734) = 0x4f6bf4u;
        UI_GU32(0x004f6744) = ebp1;
        UI_GU32(0x004f6750) = eax1;
        UI_GU32(0x004f6738) = 0x14u;
        UI_GU32(0x004f6748) = 0x46u;
        UI_GU32(0x004f674c) = 0x10u;
        UI_GU32(0x004f6758) = 0xe77c73bcu;
        UI_GU32(0x004f675c) = 0x46u;
        UI_GU32(0x004f6760) = 0xfu;
        UI_GU32(0x004f6764) = 0x4f6c00u;
        UI_GU32(0x004f6768) = 0x4f6c10u;
        UI_GU32(0x004f6754) = eax1;
        UI_GU32(0x004f676c) = 0x14u;
        UI_GU32(0x004f6774) = edx1;
        UI_GU32(0x004f677c) = 0x2fu;
        UI_GU32(0x004f6784) = ecx1;
        UI_GU32(0x004f6788) = ecx1;
        UI_GU32(0x004f6770) = 0xe77c73bcu;
        UI_GU32(0x004f6778) = 0x21041084u;
        UI_GU32(0x004f6780) = 0xcu;
        UI_GU32(0x004f678c) = 0x21041084u;
        UI_GU32(0x004f6794) = 0xdu;
        UI_GU32(0x004f6790) = 0x2fu;
        UI_GU32(0x004f67a4) = edx1;
        UI_GU32(0x004f67a8) = ecx1;
        UI_GU32(0x004f67ac) = esi1;
        UI_GU32(0x004f6798) = 0x4f6c1cu;
        const uint32_t ebx1 = UI_GU32(0x00578cf4);
        UI_GU32(0x004f67b0) = 0u;
        UI_GU32(0x004f67b8) = ebx1;
        UI_GU32(0x004f67bc) = ecx1;
        UI_GU32(0x004f67c0) = esi1;
        UI_GU32(0x004f67c4) = 0u;
        UI_GU32(0x004f679c) = 0x4f6c28u;
        UI_GU32(0x004f67a0) = 0x14u;
        UI_GU32(0x004f67b4) = 0x4u;
        UI_GU32(0x004f67c8) = 0x4u;
        UI_GU32(0x004f67cc) = 0x4f6c34u;
        UI_GU32(0x004f67dc) = edx1;
        UI_GU32(0x004f67e0) = esi1;
        UI_GU32(0x004f67d0) = 0x4f6c40u;
        UI_GU32(0x004f67d4) = 0x14u;
        UI_GU32(0x004f67ec) = ecx1;
        UI_GU32(0x004f67f0) = ecx1;
        UI_GU32(0x004f67d8) = 0xe77c73bcu;
        UI_GU32(0x004f67e4) = 0x1cu;
        UI_GU32(0x004f67e8) = 0x19u;
        UI_GU32(0x004f67f8) = 0x1cu;
        UI_GU32(0x004f67fc) = 0x1au;
        UI_GU32(0x004f6800) = 0x4f6c4cu;
        UI_GU32(0x004f67f4) = esi1;
        UI_GU32(0x004f6810) = edx1;
        UI_GU32(0x004f6804) = 0x4f6c58u;
        UI_GU32(0x004f6808) = 0x21u;
        UI_GU32(0x004f680c) = 0xe77c73bcu;
        UI_GU32(0x004f6814) = eax1;
        UI_GU32(0x004f6824) = edx1;
        UI_GU32(0x004f6818) = 0x14u;
        UI_GU32(0x004f681c) = 0xau;
        UI_GU32(0x004f6820) = 0xe77c73bcu;
        UI_GU32(0x004f682c) = 0x14u;
        UI_GU32(0x004f6830) = 0xau;
        UI_GU32(0x004f6834) = 0x4f6c64u;
        UI_GU32(0x004f6828) = eax1;
        UI_GU32(0x004f6844) = edx1;
        UI_GU32(0x004f6838) = 0x4f6c70u;
        UI_GU32(0x004f683c) = 0x21u;
        UI_GU32(0x004f6840) = 0xe77c73bcu;
        UI_GU32(0x004f6848) = eax1;
        UI_GU32(0x004f6858) = edx1;
        UI_GU32(0x004f684c) = 0x14u;
        UI_GU32(0x004f6850) = 0x9u;
        UI_GU32(0x004f6854) = 0xe77c73bcu;
        UI_GU32(0x004f685c) = eax1;
        UI_GU32(0x004f6860) = 0x14u;
        UI_GU32(0x004f6864) = 0x9u;
        UI_GU32(0x004f6874) = edx1;
        UI_GU32(0x004f6878) = edx1;
        UI_GU32(0x004f6868) = 0x4f6c7cu;
        UI_GU32(0x004f686c) = 0x4f6c88u;
        UI_GU32(0x004f6870) = 0x9u;
        UI_GU32(0x004f6880) = 0x4u;
        UI_GU32(0x004f687c) = eax1;
        UI_GU32(0x004f6890) = eax1;
        UI_GU32(0x004f6884) = 0x1u;
        UI_GU32(0x004f6888) = 0xe77c73bcu;
        UI_GU32(0x004f688c) = 0xe77c73bcu;
        UI_GU32(0x004f6894) = 0x4u;
        UI_GU32(0x004f689c) = 0u;
        UI_GU32(0x004f68a8) = edx1;
        UI_GU32(0x004f68ac) = edx1;
        UI_GU32(0x004f68b0) = eax1;
        UI_GU32(0x004f6898) = 0x1u;
        UI_GU32(0x004f68a0) = 0x4f6c94u;
        UI_GU32(0x004f68bc) = eax1;
        UI_GU32(0x004f68c0) = eax1;
        UI_GU32(0x004f68a4) = 0x9u;
        UI_GU32(0x004f68b4) = 0x4u;
        UI_GU32(0x004f68c4) = ebp1;
        UI_GU32(0x004f68d0) = 0u;
        UI_GU32(0x004f68b8) = 0x1u;
        UI_GU32(0x004f68c8) = 0x4u;
        UI_GU32(0x004f68dc) = ecx1;
        UI_GU32(0x004f68e0) = ecx1;
        UI_GU32(0x004f68e4) = eax1;
        UI_GU32(0x004f68e8) = 0u;
        UI_GU32(0x004f68cc) = 0x1u;
        UI_GU32(0x004f68f0) = ecx1;
        UI_GU32(0x004f68f4) = ecx1;
        UI_GU32(0x004f68f8) = eax1;
        UI_GU32(0x004f68fc) = 0u;
        UI_GU32(0x004f68d4) = 0x4f6ca0u;
        UI_GU32(0x004f68d8) = 0x21u;
        UI_GU32(0x004f68ec) = 0xau;
        UI_GU32(0x004f6904) = 0u;
        UI_GU32(0x004f6910) = edx1;
        UI_GU32(0x004f6914) = ecx1;
        UI_GU32(0x004f6918) = eax1;
        UI_GU32(0x004f6900) = 0xau;
        const uint32_t ebp2 = UI_GU32(0x00578ee8);
        UI_GU32(0x004f691c) = 0u;
        UI_GU32(0x004f6924) = ebp2;
        UI_GU32(0x004f6928) = ecx1;
        UI_GU32(0x004f692c) = eax1;
        UI_GU32(0x004f6930) = 0u;
        UI_GU32(0x004f6908) = 0x4f6cacu;
        UI_GU32(0x004f690c) = 0x21u;
        UI_GU32(0x004f6920) = 0xau;
        UI_GU32(0x004f6938) = 0u;
        UI_GU32(0x004f6944) = edx1;
        UI_GU32(0x004f6934) = 0xau;
        UI_GU32(0x004f693c) = 0x4f6cb8u;
        UI_GU32(0x004f6940) = 0x22u;
        UI_GU32(0x004f6948) = ecx1;
        UI_GU32(0x004f694c) = eax1;
        UI_GU32(0x004f6950) = 0u;
        UI_GU32(0x004f6958) = ebp2;
        UI_GU32(0x004f695c) = ecx1;
        UI_GU32(0x004f6960) = eax1;
        UI_GU32(0x004f6964) = 0u;
        UI_GU32(0x004f696c) = 0u;
        UI_GU32(0x004f6954) = 0xau;
        UI_GU32(0x004f6978) = ecx1;
        UI_GU32(0x004f697c) = ecx1;
        UI_GU32(0x004f6980) = eax1;
        UI_GU32(0x004f6984) = 0u;
        UI_GU32(0x004f6988) = 0u;
        UI_GU32(0x004f698c) = ecx1;
        UI_GU32(0x004f6990) = ecx1;
        UI_GU32(0x004f6994) = eax1;
        UI_GU32(0x004f6998) = 0u;
        UI_GU32(0x004f699c) = 0u;
        UI_GU32(0x004f6968) = 0xau;
        UI_GU32(0x004f6970) = 0x4f6cc4u;
        UI_GU32(0x004f69a0) = 0u;
        UI_GU32(0x004f69ac) = ecx1;
        UI_GU32(0x004f69b0) = ecx1;
        UI_GU32(0x004f69b4) = eax1;
        UI_GU32(0x004f69b8) = 0u;
        UI_GU32(0x004f69bc) = 0u;
        UI_GU32(0x004f69c0) = ecx1;
        UI_GU32(0x004f69c4) = ecx1;
        UI_GU32(0x004f69c8) = eax1;
        UI_GU32(0x004f69cc) = 0u;
        UI_GU32(0x004f69d0) = 0u;
        UI_GU32(0x004f69d4) = 0u;
        UI_GU32(0x004f6974) = 0x24u;
        UI_GU32(0x004f69a4) = 0x4f6cd0u;
        UI_GU32(0x004f69e0) = ecx1;
        UI_GU32(0x004f69e4) = ecx1;
        UI_GU32(0x004f69a8) = 0x21u;
        UI_GU32(0x004f69ec) = 0u;
        UI_GU32(0x004f69f0) = 0u;
        UI_GU32(0x004f69f4) = ebx1;
        UI_GU32(0x004f69f8) = ecx1;
        UI_GU32(0x004f69d8) = 0x4f6cdcu;
        UI_GU32(0x004f6a00) = 0u;
        UI_GU32(0x004f6a04) = 0u;
        UI_GU32(0x004f69dc) = 0x9u;
        UI_GU32(0x004f69e8) = 0x21041084u;
        UI_GU32(0x004f6a08) = 0u;
        UI_GU32(0x004f6a14) = ebp2;
        UI_GU32(0x004f6a18) = ecx1;
        UI_GU32(0x004f6a1c) = eax1;
        UI_GU32(0x004f6a20) = 0u;
        UI_GU32(0x004f6a24) = 0u;
        UI_GU32(0x004f6a28) = ebx1;
        UI_GU32(0x004f6a2c) = ecx1;
        UI_GU32(0x004f6a30) = esi1;
        UI_GU32(0x004f6a34) = 0u;
        UI_GU32(0x004f6a38) = 0u;
        UI_GU32(0x004f6a3c) = 0u;
        UI_GU32(0x004f69fc) = 0x21041084u;
        UI_GU32(0x004f6a0c) = 0x4f6ce8u;
        UI_GU32(0x004f6a10) = 0x14u;
        UI_GU32(0x004f6a40) = 0x4f6cf4u;
        UI_GU32(0x004f6a48) = ebp2;
        UI_GU32(0x004f6a4c) = ecx1;
        UI_GU32(0x004f6a50) = eax1;
        UI_GU32(0x004f6a54) = 0u;
        UI_GU32(0x004f6a58) = 0u;
        UI_GU32(0x004f6a5c) = ebx1;
        UI_GU32(0x004f6a60) = ecx1;
        UI_GU32(0x004f6a64) = esi1;
        UI_GU32(0x004f6a68) = 0u;
        UI_GU32(0x004f6a6c) = 0u;
        UI_GU32(0x004f6a70) = 0u;
        UI_GU32(0x004f6a7c) = ebp2;
        UI_GU32(0x004f6a80) = ecx1;
        UI_GU32(0x004f6a84) = eax1;
        UI_GU32(0x004f6a88) = 0u;
        UI_GU32(0x004f6a8c) = 0u;
        UI_GU32(0x004f6a90) = ebx1;
        UI_GU32(0x004f6a94) = ecx1;
        UI_GU32(0x004f6a98) = esi1;
        UI_GU32(0x004f6a9c) = 0u;
        UI_GU32(0x004f6aa0) = 0u;
        UI_GU32(0x004f6aa4) = 0u;
        UI_GU32(0x004f6a44) = 0x9u;
        UI_GU32(0x004f6a74) = 0x4f6d00u;
        UI_GU32(0x004f6ab0) = ebp2;
        UI_GU32(0x004f6ab4) = ecx1;
        UI_GU32(0x004f6ab8) = eax1;
        UI_GU32(0x004f6abc) = 0u;
        UI_GU32(0x004f6ac0) = 0u;
        UI_GU32(0x004f6ac4) = ebx1;
        UI_GU32(0x004f6ac8) = ecx1;
        UI_GU32(0x004f6acc) = esi1;
        UI_GU32(0x004f6ad0) = 0u;
        UI_GU32(0x004f6ad4) = 0u;
        UI_GU32(0x004f6a78) = 0xau;
        UI_GU32(0x004f6aa8) = 0x4f6d0cu;
        UI_GU32(0x004f6ad8) = 0u;
        UI_GU32(0x004f6ae4) = ebp2;
        UI_GU32(0x004f6ae8) = ecx1;
        UI_GU32(0x004f6aec) = eax1;
        UI_GU32(0x004f6aac) = 0xau;
        UI_GU32(0x004f6af8) = ebx1;
        UI_GU32(0x004f6af0) = 0x17u;
        UI_GU32(0x004f6afc) = ecx1;
        UI_GU32(0x004f6b00) = esi1;
        UI_GU32(0x004f6b04) = 0x17u;
        UI_GU32(0x004f6adc) = 0x4f6d18u;
        UI_GU32(0x004f6ae0) = 0x14u;
        UI_GU32(0x004f6af4) = 0x8u;
        UI_GU32(0x004f6b08) = 0x8u;
        const uint32_t ebp3 = UI_GU32(0x00578d04);
        UI_GU32(0x004f6b1c) = ecx1;
        UI_GU32(0x004f6b18) = ebp3;
        UI_GU32(0x004f6b20) = eax1;
        UI_GU32(0x004f6b0c) = 0x4f6d24u;
        UI_GU32(0x004f6b10) = 0x4f6d30u;
        UI_GU32(0x004f6b14) = 0x14u;
        UI_GU32(0x004f6b28) = 0x8u;
        UI_GU32(0x004f6b24) = 0x17u;
        UI_GU32(0x004f6b2c) = ebx1;
        UI_GU32(0x004f6b30) = ecx1;
        UI_GU32(0x004f6b34) = esi1;
        UI_GU32(0x004f6b3c) = 0x8u;
        UI_GU32(0x004f6b38) = 0x17u;
        UI_GU32(0x004f6b4c) = ecx1;
        UI_GU32(0x004f6b50) = ecx1;
        UI_GU32(0x004f6b54) = eax1;
        UI_GU32(0x004f6b40) = 0x4f6d3cu;
        UI_GU32(0x004f6b58) = 0u;
        UI_GU32(0x004f6b60) = ecx1;
        UI_GU32(0x004f6b64) = ecx1;
        UI_GU32(0x004f6b68) = eax1;
        UI_GU32(0x004f6b44) = 0x4f6d4cu;
        UI_GU32(0x004f6b6c) = 0u;
        UI_GU32(0x004f6b74) = 0u;
        UI_GU32(0x004f6b48) = 0x24u;
        UI_GU32(0x004f6b5c) = 0xau;
        UI_GU32(0x004f6b70) = 0xau;
    }
    for (uint32_t d = S_STYLE_DESCS, i = 0; d < S_STYLE_DESCS_END; d += 0x34, i++)
        ccall<void>(F_UIAddStyle, (int32_t)i, (const UIStyleDesc*)(uintptr_t)d);
    ccall<void>(F_WidgetBegin);
}
static void fp_UIBegin(Footprint& f) { f.replay_only = "loads stamps and fonts, allocates palettes, the sync object"; }
PORT_FN(0x004779a0, "UIBegin", UIBegin_n, fp_UIBegin)

static void __cdecl UIEnd_n() {
    ccall<void>(F_WidgetEnd);
    for (int32_t i = 0; i < 0x18; i++) ccall<void>(F_UIRemoveStyle, i);
    ccall<void>(F_UIStyleEnd);
    for (uint32_t a = S_UI_STAMPS; a < S_UI_STAMPS + 0x10; a += 4) ccall<void>(F_gxForgetStamp, UI_GP(void, a));
    ccall<void>(F_SingleEnd, UI_G32(S_UI_SYNC), (const char*)0, (int32_t)0);
}
static void fp_UIEnd(Footprint& f) { f.replay_only = "frees stamps, fonts, palettes, the sync object"; }
PORT_FN(0x00478380, "UIEnd", UIEnd_n, fp_UIEnd)

// _UIUpdateTime: the seconds since the last call (PTimeNow's ms * 0.001f), at most 0.25 (0.25 for NaN)
static void __cdecl UIUpdateTime_n() {
    const int32_t now = ccall<int32_t>(F_PTimeNow);
    const int32_t ms = (int32_t)((uint32_t)now - (uint32_t)UI_G32(S_UI_LAST_TIME));
    const double t = D(ms) * D(bits_f(0x3a83126f));
    UI_GF(S_UI_DT) = D(bits_f(0x3e800000)) > t ? (float)t : bits_f(0x3e800000);
    UI_G32(S_UI_LAST_TIME) = now;
}
static void fp_UIUpdateTime(Footprint& f) { f.replay_only = "reads the wall clock (PTimeNow)"; }
PORT_FN(0x004785a0, "_UIUpdateTime", UIUpdateTime_n, fp_UIUpdateTime)

static float __cdecl UIDeltaT_n() { return UI_GF(S_UI_DT); }
static void fp_UIDeltaT(Footprint&) {}
PORT_FN(0x00479270, "UIDeltaT", UIDeltaT_n, fp_UIDeltaT)

// =====================================================================================================================
// _UIAddItems
// =====================================================================================================================
static __forceinline void add(WidgetWindow* win, void* w) { ccall<void>(F_WidgetAddItem, win, w); }
// a button's Escape (id -1) / Return (id -2) hot key, with the button's callback
static __forceinline void* hotkey(int32_t id, uint16_t key, void* cb) {
    void* p = ccall<void*>(F_MemAlloc, 0x234);
    if (!p) return 0;
    tcall<Widget*>(F_Widget_ctor, p, (int32_t)0, (int32_t)0, (int32_t)0, (int32_t)0);
    HotKey* h = (HotKey*)p;
    h->id = id;
    h->cb = (UICallback)cb;
    h->vtbl = (const void*)(uintptr_t)VT_HotKey;
    h->key = key;
    return p;
}
// the direction of an arrow / scroll button from its item's style (0 or 2: up / left, -1; else 1)
static __forceinline int32_t arrow_dir(int32_t style) {
    const uint32_t a = (style - 2) == 0 ? 1u : 0u;
    const uint32_t b = style == 0 ? 1u : 0u;
    return (a | b) == 0 ? 1 : -1;
}
// a slider's or arrow's range: nothing if |lo - hi| is at most FLT_EPSILON (or NaN)
static __forceinline bool no_range(const volatile UIDialogItem* it) {
    double d = D(*(const volatile float*)&it->lo) - D(*(const volatile float*)&it->hi);
    d = d < 0 ? -d : d;                                     // fabs (the sign bit; NaN stays NaN)
    return !(d > D(bits_f(0x34000000)));
}
static void __cdecl UIAddItems_n(WidgetWindow* win, UIDialogItem* items) {
    volatile UIDialogItem* it = items;
    for (; it->type != 0; it++) {
        const int32_t type = it->type;
        const uint32_t k = (uint32_t)(type - 1);
        if (k > 0x16) {
            UI_LogReport((const char*)0x004f6ec8, type);
            continue;
        }
        switch (k + 1) {
        case 1:                                                  // a group: entered (id 0) or left
            if (it->id == 0) ccall<void>(F_WidgetEnterGroup, win, it->data);
            else ccall<void>(F_WidgetLeaveGroup, win, it->data);
            break;
        case 2:                                                  // Button (id -1 / -2: Escape / Return fire it too)
        case 3: {                                                // BButton
            void* p = ccall<void*>(F_MemAlloc, type == 2 ? 0x290 : 0x238);
            void* w = 0;
            if (p) {
                const uint8_t repeat = it->i1c != 0 ? 1 : 0;
                if (type == 2)
                    w = tcall<void*>(F_Button_ctor, p, (int32_t)it->id, (int32_t)it->x, (int32_t)it->y, (const char*)it->text,
                                     (void*)it->data, (int32_t)it->style, (uint32_t)0, (uint32_t)repeat);
                else
                    w = tcall<void*>(F_BButton_ctor, p, (int32_t)it->id, (int32_t)it->x, (int32_t)it->y, (const char*)it->text,
                                     (void*)it->data, (uint32_t)0, (uint32_t)repeat);
            }
            add(win, w);
            if (it->id == -1) {
                void* h = ccall<void*>(F_MemAlloc, 0x234);
                if (h) {
                    void* cb = it->data;
                    tcall<Widget*>(F_Widget_ctor, h, (int32_t)0, (int32_t)0, (int32_t)0, (int32_t)0);
                    ((HotKey*)h)->id = -1;
                    ((HotKey*)h)->cb = (UICallback)cb;
                    ((HotKey*)h)->vtbl = (const void*)(uintptr_t)VT_HotKey;
                    ((HotKey*)h)->key = 0x1b;
                }
                add(win, h);
            }
            if (it->id == -2) {
                void* h = ccall<void*>(F_MemAlloc, 0x234);
                if (h) {
                    void* cb = it->data;
                    tcall<Widget*>(F_Widget_ctor, h, (int32_t)0, (int32_t)0, (int32_t)0, (int32_t)0);
                    ((HotKey*)h)->id = -2;
                    ((HotKey*)h)->cb = (UICallback)cb;
                    ((HotKey*)h)->vtbl = (const void*)(uintptr_t)VT_HotKey;
                    ((HotKey*)h)->key = 0xd;
                }
                add(win, h);
            }
            break;
        }
        case 4: {                                                // HotKey: key x, id, callback data
            void* h = ccall<void*>(F_MemAlloc, 0x234);
            if (h) {
                const uint16_t key = *(const volatile uint16_t*)&it->x;
                void* cb = it->data;
                const int32_t id = it->id;
                tcall<Widget*>(F_Widget_ctor, h, (int32_t)0, (int32_t)0, (int32_t)0, (int32_t)0);
                ((HotKey*)h)->id = id;
                ((HotKey*)h)->cb = (UICallback)cb;
                ((HotKey*)h)->vtbl = (const void*)(uintptr_t)VT_HotKey;
                ((HotKey*)h)->key = key;
            }
            add(win, h);
            break;
        }
        case 5: {                                                // StaticText
            void* p = ccall<void*>(F_MemAlloc, 0x340);
            void* w = 0;
            if (p) w = tcall<void*>(F_StaticText_ctor, p, (int32_t)it->x, (int32_t)it->y, (const char*)it->text, (int32_t)it->style);
            add(win, w);
            break;
        }
        case 6:                                                  // Numeric
        case 7: {                                                // IntNumeric
            void* p = ccall<void*>(F_MemAlloc, 0x294);
            void* w = 0;
            if (p)
                w = tcall<void*>(type == 6 ? F_Numeric_ctor : F_IntNumeric_ctor, p, (int32_t)it->x, (int32_t)it->y, (const void*)it->data,
                                 *(const volatile uint32_t*)&it->lo, *(const volatile uint32_t*)&it->hi, (const char*)it->text,
                                 (int32_t)it->style);
            add(win, w);
            break;
        }
        case 8: {                                                // Multi
            void* p = ccall<void*>(F_MemAlloc, 0x2ac);
            void* w = 0;
            if (p) w = tcall<void*>(F_Multi_ctor, p, (int32_t)it->x, (int32_t)it->y, (void*)it->data, (const char*)it->text, (int32_t)it->style);
            add(win, w);
            break;
        }
        case 9: {                                                // Input: w, h, buffer data, width i1c, lines text
            void* p = ccall<void*>(F_MemAlloc, 0xa48);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_Input_ctor, p, (int32_t)it->x, (int32_t)it->y, (int32_t)it->w, (int32_t)it->h, (char*)it->data,
                                 (int32_t)it->i1c, (int32_t)(uintptr_t)it->text, (int32_t)it->style, (uint32_t)0);
            add(win, w);
            break;
        }
        case 10: {                                               // CDetect: prompt text, Control* data
            void* p = ccall<void*>(F_MemAlloc, 0xa68);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_CDetect_ctor, p, (int32_t)it->x, (int32_t)it->y, (int32_t)it->w, (int32_t)it->h, (const char*)it->text,
                                 (void*)it->data, (int32_t)it->style);
            add(win, w);
            break;
        }
        case 11: {                                               // CheckBox
            void* p = ccall<void*>(F_MemAlloc, 0x338);
            void* w = 0;
            if (p) w = tcall<void*>(F_CheckBox_ctor, p, (int32_t)it->x, (int32_t)it->y, (const char*)it->text, (void*)it->data, (int32_t)it->style);
            add(win, w);
            break;
        }
        case 12: {                                               // RadioButton: value i1c, var data, group mode w < 0
            void* p = ccall<void*>(F_MemAlloc, 0x340);
            void* w = 0;
            if (p) {
                const uint8_t mode = it->w < 0 ? 1 : 0;
                w = tcall<void*>(F_RadioButton_ctor, p, (int32_t)it->x, (int32_t)it->y, (const char*)it->text, (int32_t)it->i1c,
                                 (void*)it->data, (int32_t)it->style, (uint32_t)mode);
            }
            add(win, w);
            break;
        }
        case 13: {                                               // BRadioButton
            void* p = ccall<void*>(F_MemAlloc, 0x234);
            void* w = 0;
            if (p) w = tcall<void*>(F_BRadioButton_ctor, p, (int32_t)it->x, (int32_t)it->y, (const char*)it->text, (int32_t)it->i1c, (void*)it->data);
            add(win, w);
            break;
        }
        case 14: {                                               // StampRoll
            void* p = ccall<void*>(F_MemAlloc, 0x230);
            void* w = 0;
            if (p) w = tcall<void*>(F_StampRoll_ctor, p, (int32_t)it->x, (int32_t)it->y, (const char*)it->text, (void*)it->data);
            add(win, w);
            break;
        }
        case 15: {                                               // Slider (steps i1c), with an arrow button each side
            if (no_range(it)) break;
            if (it->i1c == 0) break;
            void* p = ccall<void*>(F_MemAlloc, 0x254);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_Slider_ctor, p, (int32_t)it->x, (int32_t)it->y, (int32_t)it->w, (int32_t)it->h, (void*)it->data,
                                 *(const volatile uint32_t*)&it->lo, *(const volatile uint32_t*)&it->hi, (int32_t)it->i1c);
            add(win, w);
            p = ccall<void*>(F_MemAlloc, 0x24c);
            w = 0;
            if (p) {
                void* st = UI_GP(void, S_UI_STAMPS);
                w = tcall<void*>(F_ArrowButton_ctor, p, (int32_t)(it->x - 0x13), (int32_t)(it->y + 1), (void*)it->data,
                                 *(const volatile uint32_t*)&it->lo, *(const volatile uint32_t*)&it->hi, iabs(it->i1c), (int32_t)-1, st);
            }
            add(win, w);
            p = ccall<void*>(F_MemAlloc, 0x24c);
            w = 0;
            if (p) {
                void* st = UI_GP(void, S_UI_STAMPS + 4);
                w = tcall<void*>(F_ArrowButton_ctor, p, (int32_t)(it->x + it->w + 0xd), (int32_t)(it->y + 1), (void*)it->data,
                                 *(const volatile uint32_t*)&it->lo, *(const volatile uint32_t*)&it->hi, iabs(it->i1c), (int32_t)1, st);
            }
            add(win, w);
            break;
        }
        case 16: {                                               // ArrowButton (the stamp and direction by style)
            if (no_range(it)) break;
            if (it->i1c == 0) break;
            void* p = ccall<void*>(F_MemAlloc, 0x24c);
            void* w = 0;
            if (p) {
                const int32_t s = it->style;
                void* st = UI_GP(void, S_UI_STAMPS + 4 * (uint32_t)s);
                w = tcall<void*>(F_ArrowButton_ctor, p, (int32_t)it->x, (int32_t)it->y, (void*)it->data, *(const volatile uint32_t*)&it->lo,
                                 *(const volatile uint32_t*)&it->hi, (int32_t)it->i1c, arrow_dir(s), st);
            }
            add(win, w);
            break;
        }
        case 17: {                                               // a vertical scroll bar with its two buttons
            void* p = ccall<void*>(F_MemAlloc, 0x240);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_ScrollBar_ctor, p, (int32_t)it->x, (int32_t)(it->y + 0x13), (int32_t)it->w, (int32_t)(it->h - 0x26),
                                 (void*)it->data, (int32_t)0);
            add(win, w);
            p = ccall<void*>(F_MemAlloc, 0x240);
            w = 0;
            if (p) w = tcall<void*>(F_ScrollButton_ctor, p, (int32_t)it->x, (int32_t)it->y, (void*)it->data, (int32_t)-1, UI_GP(void, S_UI_STAMPS + 8));
            add(win, w);
            p = ccall<void*>(F_MemAlloc, 0x240);
            w = 0;
            if (p)
                w = tcall<void*>(F_ScrollButton_ctor, p, (int32_t)it->x, (int32_t)(it->h + it->y - 0x12), (void*)it->data, (int32_t)1,
                                 UI_GP(void, S_UI_STAMPS + 0xc));
            add(win, w);
            break;
        }
        case 18: {                                               // a horizontal one
            void* p = ccall<void*>(F_MemAlloc, 0x240);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_ScrollBar_ctor, p, (int32_t)(it->x + 0x13), (int32_t)it->y, (int32_t)(it->w - 0x26), (int32_t)it->h,
                                 (void*)it->data, (int32_t)1);
            add(win, w);
            p = ccall<void*>(F_MemAlloc, 0x240);
            w = 0;
            if (p) w = tcall<void*>(F_ScrollButton_ctor, p, (int32_t)it->x, (int32_t)it->y, (void*)it->data, (int32_t)-1, UI_GP(void, S_UI_STAMPS));
            add(win, w);
            p = ccall<void*>(F_MemAlloc, 0x240);
            w = 0;
            if (p)
                w = tcall<void*>(F_ScrollButton_ctor, p, (int32_t)(it->x + it->w - 0x12), (int32_t)it->y, (void*)it->data, (int32_t)1,
                                 UI_GP(void, S_UI_STAMPS + 4));
            add(win, w);
            break;
        }
        case 19: {                                               // ScrollButton (the stamp and direction by style)
            void* p = ccall<void*>(F_MemAlloc, 0x240);
            void* w = 0;
            if (p) {
                const int32_t s = it->style;
                void* st = UI_GP(void, S_UI_STAMPS + 4 * (uint32_t)s);
                w = tcall<void*>(F_ScrollButton_ctor, p, (int32_t)it->x, (int32_t)it->y, (void*)it->data, arrow_dir(s), st);
            }
            add(win, w);
            break;
        }
        case 20: {                                               // ListBox: list data, selection sel, axis text
            void* p = ccall<void*>(F_MemAlloc, 0x24c);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_ListBox_ctor, p, (int32_t)it->x, (int32_t)it->y, (int32_t)it->w, (int32_t)it->h, (void*)it->data,
                                 (void*)it->sel, (void*)it->text);
            add(win, w);
            break;
        }
        case 21: {                                               // DropList
            void* p = ccall<void*>(F_MemAlloc, 0x254);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_DropList_ctor, p, (int32_t)it->x, (int32_t)it->y, (int32_t)it->w, (int32_t)it->h, (void*)it->data,
                                 (void*)it->sel);
            add(win, w);
            break;
        }
        case 22: {                                               // LineWidget: colour i1c
            void* p = ccall<void*>(F_MemAlloc, 0x23c);
            void* w = 0;
            if (p)
                w = tcall<void*>(F_LineWidget_ctor, p, (int32_t)it->x, (int32_t)it->y, (int32_t)it->w, (int32_t)it->h, (uint32_t)it->i1c);
            add(win, w);
            break;
        }
        case 23: {                                               // CustomWidget around the UICustomControl in data
            void* p = ccall<void*>(F_MemAlloc, 0x22c);
            if (p) {
                UICustomControl* c = (UICustomControl*)it->data;
                const int32_t h = it->h, w = it->w, y = it->y, x = it->x;
                tcall<Widget*>(F_Widget_ctor, p, x, y, w + x, h + y);
                CustomWidget* cw = (CustomWidget*)p;
                cw->vtbl = (const void*)(uintptr_t)VT_CustomWidget;
                cw->ctrl = c;
                c->widget = cw;
                c->x = x;
                c->y = y;
                c->w = w;
                c->h = h;
            }
            add(win, p);
            break;
        }
        }
    }
}
static void fp_UIAddItems(Footprint& f, WidgetWindow*, UIDialogItem*) { f.replay_only = "creates widgets (allocates, loads stamps)"; }
PORT_FN(0x004785f0, "_UIAddItems", UIAddItems_n, fp_UIAddItems)

// =====================================================================================================================
// UIDoDialog, UIDoMenu, the groups
// =====================================================================================================================
// UIDoDialog: a window (centred where x / y is -999; a title bar you can drag unless it's full screen or placed), the
// title, the items, the default focus; run with the dialog's idle function; its exit code
static int32_t __cdecl UIDoDialog_n(const UIDialog* dlg, int32_t w, int32_t h, int32_t x, int32_t y, int32_t ui3d) {
    ccall<void>(F_MouseClear);
    ccall<void>(F_KeyClear);
    const uint32_t saved = UI_GU32(S_DIALOG_IDLE);
    uint8_t framed;
    if (w == UI_G32(S_SCREEN_W) && UI_G32(S_SCREEN_H) == h) framed = 0;
    else framed = 1;
    if (x == -999) x = (UI_G32(S_SCREEN_W) - w) / 2;
    else framed = 0;
    if (y == -999) y = (UI_G32(S_SCREEN_H) - h) / 2;
    else framed = 0;
    const volatile UIDialog* d = dlg;
    WidgetWindow* win = ccall<WidgetWindow*>(F_WidgetCreateWindow, (const char*)d->background, x, y, w, h, ui3d);
    if (framed) {
        void* p = ccall<void*>(F_MemAlloc, 0x340);
        void* t = 0;
        if (p) t = tcall<void*>(F_StaticText_ctor, p, (int32_t)9, (int32_t)3, (const char*)d->title, (int32_t)0xc);
        add(win, t);
        void* b = ccall<void*>(F_MemAlloc, 0x234);
        if (b) {
            tcall<Widget*>(F_Widget_ctor, b, (int32_t)4, (int32_t)1, w, (int32_t)0x11);
            ((TitleBar*)b)->vtbl = (const void*)(uintptr_t)VT_TitleBar;
            ((TitleBar*)b)->dragging = 0;
        }
        add(win, b);
    } else {
        void* p = ccall<void*>(F_MemAlloc, 0x340);
        void* t = 0;
        if (p) t = tcall<void*>(F_StaticText_ctor, p, (int32_t)0x1d, (int32_t)0x16, (const char*)d->title, (int32_t)0x10);
        add(win, t);
    }
    ccall<void>(F_UIAddItems, win, (UIDialogItem*)d->items);
    ccall<void>(F_WidgetSetDefault, win, (int32_t)d->default_id);
    UI_GU32(S_DIALOG_IDLE) = (uint32_t)(uintptr_t)d->idle;
    ccall<void>(F_WidgetSetIdleFunction, win, (uint32_t)F_dialog_idle_func);
    const int32_t r = ccall<int32_t>(F_WidgetExecuteWindow, win);
    ccall<void>(F_WidgetDestroyWindow, win);
    UI_GU32(S_DIALOG_IDLE) = saved;
    return r;
}
static void fp_UIDoDialog(Footprint& f, const UIDialog*, int32_t, int32_t, int32_t, int32_t, int32_t) { f.replay_only = "a dialog: runs modally (its own input loop)"; }
PORT_FN(0x00478ff0, "UIDoDialog", UIDoDialog_n, fp_UIDoDialog)

// dialog_idle_func: the running dialog's idle function (code -2 unless it says otherwise); nonzero exits
static void __cdecl dialog_idle_func_n() {
    volatile int32_t code;
    UIIdle fn = (UIIdle)(uintptr_t)UI_GU32(S_DIALOG_IDLE);
    code = -2;
    if (!fn) return;
    if (!fn((int32_t*)&code)) return;
    ccall<void>(F_WidgetExit, (int32_t)code);
}
static void fp_dialog_idle_func(Footprint& f) { f.replay_only = "calls the dialog's idle function (menu code)"; }
PORT_FN(0x00479170, "dialog_idle_func", dialog_idle_func_n, fp_dialog_idle_func)

// UIDoMenu: a full-screen window: the title, a menu button per item (49 pixels apart from y 142), a Back button (-2),
// Escape (-1); the chosen id
static int32_t __cdecl UIDoMenu_n(const UIMenu* menu) {
    const volatile UIMenu* m = menu;
    const int32_t sh = UI_G32(S_SCREEN_H), sw = UI_G32(S_SCREEN_W);
    WidgetWindow* win = ccall<WidgetWindow*>(F_WidgetCreateWindow, (const char*)m->background, (int32_t)0, (int32_t)0, sw, sh, (int32_t)0);
    void* p = ccall<void*>(F_MemAlloc, 0x340);
    void* t = 0;
    if (p) t = tcall<void*>(F_StaticText_ctor, p, (int32_t)0x1e, (int32_t)0x18, (const char*)m->title, (int32_t)0x10);
    add(win, t);
    if (*(const volatile uint32_t*)m->items != 0) {
        int32_t y = 0x8e;
        uint32_t off = 0;
        do {
            void* b = ccall<void*>(F_MemAlloc, 0x290);
            if (b) {
                const volatile uint32_t* e = (const volatile uint32_t*)((const uint8_t*)m->items + off);
                tcall<void*>(F_Button_ctor, b, (int32_t)e[1], (int32_t)0x1ef, y, (const char*)(uintptr_t)e[0], (void*)0, (int32_t)2,
                             (uint32_t)1, (uint32_t)0);
                ((Widget*)b)->vtbl = (const void*)(uintptr_t)VT_MenuButton;
            }
            add(win, b);
            y += 0x31;
            off += 8;
        } while (*(const volatile uint32_t*)((const uint8_t*)m->items + off) != 0);
    }
    if (m->back != 0) {
        void* b = ccall<void*>(F_MemAlloc, 0x290);
        if (b) {
            const char* back = xlate(XL_BACK);
            tcall<void*>(F_Button_ctor, b, (int32_t)-2, (int32_t)0x1ef, (int32_t)0x183, back, (void*)0, (int32_t)2, (uint32_t)1, (uint32_t)0);
            ((Widget*)b)->vtbl = (const void*)(uintptr_t)VT_MenuButton;
        }
        add(win, b);
    }
    void* h = ccall<void*>(F_MemAlloc, 0x234);
    if (h) {
        tcall<Widget*>(F_Widget_ctor, h, (int32_t)0, (int32_t)0, (int32_t)0, (int32_t)0);
        ((HotKey*)h)->id = -1;
        ((HotKey*)h)->cb = 0;
        ((HotKey*)h)->vtbl = (const void*)(uintptr_t)VT_HotKey;
        ((HotKey*)h)->key = 0x1b;
    }
    add(win, h);
    ccall<void>(F_WidgetSetDefault, win, (int32_t)m->default_id);
    const int32_t r = ccall<int32_t>(F_WidgetExecuteWindow, win);
    ccall<void>(F_WidgetDestroyWindow, win);
    return r;
}
static void fp_UIDoMenu(Footprint& f, const UIMenu*) { f.replay_only = "a menu: runs modally (its own input loop)"; }
PORT_FN(0x004783d0, "UIDoMenu", UIDoMenu_n, fp_UIDoMenu)

// the groups, on the running window (a panic without one)
static void fp_active_groups(Footprint& f, uint32_t) { ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE)); }
#define UI_GROUP_FN(NAME, FN, MSG)                                                                                  \
    static void __cdecl NAME##_n(uint32_t g) {                                                                       \
        WidgetWindow* w = ccall<WidgetWindow*>(F_WidgetGetActiveWindow);                                             \
        if (w) { ccall<void>(FN, w, (int32_t)g); return; }                                                           \
        UI_LogPanic((const char*)MSG);                                                                               \
    }
UI_GROUP_FN(UIHideGroup, F_WidgetHideGroup, 0x004f6efc)
PORT_FN(0x004791b0, "UIHideGroup", UIHideGroup_n, fp_active_groups)
UI_GROUP_FN(UIShowGroup, F_WidgetShowGroup, 0x004f6f1c)
PORT_FN(0x004791e0, "UIShowGroup", UIShowGroup_n, fp_active_groups)
UI_GROUP_FN(UIDisableGroup, F_WidgetDisableGroup, 0x004f6f3c)
PORT_FN(0x00479210, "UIDisableGroup", UIDisableGroup_n, fp_active_groups)
UI_GROUP_FN(UIEnableGroup, F_WidgetEnableGroup, 0x004f6f60)
PORT_FN(0x00479240, "UIEnableGroup", UIEnableGroup_n, fp_active_groups)
#undef UI_GROUP_FN

// =====================================================================================================================
// the stock dialogs (320 x 200, centred)
// =====================================================================================================================
// UIDoDratBox: text and one button (the "drat" text), with an idle function
static void __cdecl UIDoDratBox_n(const char* title, const char* text, UIIdle idle) {
    UIDialog dlg;
    UIDialogItem it[3];
    item(&it[0], 5, 0, 0xa0, 0x28, 0, 0, text, 0, 0, 0xe);
    const char* drat = xlate(XL_DRAT);
    item(&it[1], 2, -2, 0x70, 0x99, 0, 0, drat, 0, 0, 0);
    item_end(&it[2]);
    dialog(&dlg, title, (const char*)0x004f6f84, -2, it, idle);
    run_dialog(&dlg, 0x140, 0xc8);
}
PORT_FN(0x00479280, "UIDoDratBox", UIDoDratBox_n, fp_modal3)

// UIDoOkBox: text, OK (and Escape)
static void __cdecl UIDoOkBox_n(const char* title, const char* text) {
    UIDialog dlg;
    UIDialogItem it[4];
    item(&it[0], 5, 0, 0xa0, 0x28, 0, 0, text, 0, 0, 0xe);
    item(&it[1], 4, -2, 0x1b, 0, 0, 0, 0, 0, 0, 0);
    const char* ok = xlate(XL_OK);
    item(&it[2], 2, -2, 0x70, 0x99, 0, 0, ok, 0, 0, 0);
    item_end(&it[3]);
    dialog(&dlg, title, (const char*)0x004f6f90, -2, it, 0);
    run_dialog(&dlg, 0x140, 0xc8);
}
PORT_FN(0x004793c0, "UIDoOkBox", UIDoOkBox_n, fp_modal2)

// UIDoLongOkBox: the text word-wrapped (style 15, 284 pixels) into a 4 KB buffer
static void __cdecl UIDoLongOkBox_n(const char* title, const char* text) {
    char buf[0x1000];
    UIDialog dlg;
    UIDialogItem it[4];
    ccall<void>(F_UIStyleWordWrap, (int32_t)0xf, (int32_t)0x11c, (char*)buf, text);
    item(&it[0], 5, 0, 0xa, 0x28, 0, 0, buf, 0, 0, 0xf);
    item(&it[1], 4, -2, 0x1b, 0, 0, 0, 0, 0, 0, 0);
    const char* ok = xlate(XL_OK);
    item(&it[2], 2, -2, 0x70, 0x99, 0, 0, ok, 0, 0, 0);
    item_end(&it[3]);
    dialog(&dlg, title, (const char*)0x004f6f9c, -2, it, 0);
    run_dialog(&dlg, 0x140, 0xc8);
}
PORT_FN(0x00479570, "UIDoLongOkBox", UIDoLongOkBox_n, fp_modal2)

static uint8_t __cdecl UIDoOkCancelBox2_n(const char* title, const char* text) {
    return ccall<uint8_t>(F_UIDoOkCancelBox3, title, text, (UIIdle)0);
}
PORT_FN(0x00479740, "UIDoOkCancelBox(title,text)", UIDoOkCancelBox2_n, fp_modal2)

// UIDoOkCancelBox: OK (-2) or Cancel (-1); true for OK
static uint8_t __cdecl UIDoOkCancelBox3_n(const char* title, const char* text, UIIdle idle) {
    UIDialog dlg;
    UIDialogItem it[4];
    item(&it[0], 5, 0, 0xa0, 0x28, 0, 0, text, 0, 0, 0xe);
    const char* ok = xlate(XL_OK);
    item(&it[1], 2, -2, 9, 0x99, 0, 0, ok, 0, 0, 0);
    const char* cancel = xlate(XL_CANCEL);
    item(&it[2], 2, -1, 0xd7, 0x99, 0, 0, cancel, 0, 0, 0);
    item_end(&it[3]);
    dialog(&dlg, title, (const char*)0x004f6fa8, -2, it, idle);
    return run_dialog(&dlg, 0x140, 0xc8) + 2 == 0 ? 1 : 0;
}
PORT_FN(0x00479760, "UIDoOkCancelBox(title,text,idle)", UIDoOkCancelBox3_n, fp_modal3)

static uint8_t __cdecl UIDoLongOkCancelBox_n(const char* title, const char* text) {
    char buf[0x1000];
    UIDialog dlg;
    UIDialogItem it[4];
    ccall<void>(F_UIStyleWordWrap, (int32_t)0xf, (int32_t)0x11c, (char*)buf, text);
    item(&it[0], 5, 0, 0xa, 0x28, 0, 0, buf, 0, 0, 0xf);
    const char* ok = xlate(XL_OK);
    item(&it[1], 2, -2, 9, 0x99, 0, 0, ok, 0, 0, 0);
    const char* cancel = xlate(XL_CANCEL);
    item(&it[2], 2, -1, 0xd7, 0x99, 0, 0, cancel, 0, 0, 0);
    item_end(&it[3]);
    dialog(&dlg, title, (const char*)0x004f6fb4, -2, it, 0);
    return run_dialog(&dlg, 0x140, 0xc8) + 2 == 0 ? 1 : 0;
}
PORT_FN(0x00479940, "UIDoLongOkCancelBox", UIDoLongOkCancelBox_n, fp_modal2)

// UIDoYesNoBox: Yes (-2) / No (-3), each with its first letter as a hot key; true for Yes
static uint8_t __cdecl UIDoYesNoBox_n(const char* title, const char* text, UIIdle idle) {
    UIDialog dlg;
    UIDialogItem it[6];
    item(&it[0], 5, 0, 0xa0, 0x28, 0, 0, text, 0, 0, 0xe);
    const char* yes = xlate(XL_YES);
    item(&it[1], 2, -2, 9, 0x99, 0, 0, yes, 0, 0, 0);
    item(&it[2], 4, -2, first_key(xlate(XL_YES)), 0, 0, 0, 0, 0, 0, 0);
    const char* no = xlate(XL_NO);
    item_ctor(&it[3], 2, -3, 0xd7, 0x99, 0, 0, no, 0, 0, 0, 0, 0);
    item(&it[4], 4, -3, first_key(xlate(XL_NO)), 0, 0, 0, 0, 0, 0, 0);
    item_end(&it[5]);
    dialog(&dlg, title, (const char*)0x004f6fc0, -2, it, idle);
    return run_dialog(&dlg, 0x140, 0xc8) + 2 == 0 ? 1 : 0;
}
PORT_FN(0x00479b30, "UIDoYesNoBox", UIDoYesNoBox_n, fp_modal3)

static uint8_t __cdecl UIDoLongYesNoBox_n(const char* title, const char* text) {
    char buf[0x1000];
    UIDialog dlg;
    UIDialogItem it[6];
    ccall<void>(F_UIStyleWordWrap, (int32_t)0xf, (int32_t)0x11c, (char*)buf, text);
    item(&it[0], 5, 0, 0xa, 0x28, 0, 0, buf, 0, 0, 0xf);
    const char* yes = xlate(XL_YES);
    item(&it[1], 2, -2, 9, 0x99, 0, 0, yes, 0, 0, 0);
    item(&it[2], 4, -2, first_key(xlate(XL_YES)), 0, 0, 0, 0, 0, 0, 0);
    const char* no = xlate(XL_NO);
    item_ctor(&it[3], 2, -3, 0xd7, 0x99, 0, 0, no, 0, 0, 0, 0, 0);
    item(&it[4], 4, -3, first_key(xlate(XL_NO)), 0, 0, 0, 0, 0, 0, 0);
    item_end(&it[5]);
    dialog(&dlg, title, (const char*)0x004f6fcc, -2, it, 0);
    return run_dialog(&dlg, 0x140, 0xc8) + 2 == 0 ? 1 : 0;
}
PORT_FN(0x00479df0, "UIDoLongYesNoBox", UIDoLongYesNoBox_n, fp_modal2)

// UIDoYesNoCancelBox: Yes -2, No -3, Cancel -1: the exit code
static int32_t __cdecl UIDoYesNoCancelBox_n(const char* title, const char* text) {
    UIDialog dlg;
    UIDialogItem it[7];
    item(&it[0], 5, 0, 0xa0, 0x28, 0, 0, text, 0, 0, 0xe);
    const char* yes = xlate(XL_YES);
    item(&it[1], 2, -2, 9, 0x99, 0, 0, yes, 0, 0, 0);
    item_ctor(&it[2], 4, -2, first_key(xlate(XL_YES)), 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const char* no = xlate(XL_NO);
    item_ctor(&it[3], 2, -3, 0x70, 0x99, 0, 0, no, 0, 0, 0, 0, 0);
    item(&it[4], 4, -3, first_key(xlate(XL_NO)), 0, 0, 0, 0, 0, 0, 0);
    const char* cancel = xlate(XL_CANCEL);
    item(&it[5], 2, -1, 0xd7, 0x99, 0, 0, cancel, 0, 0, 0);
    item_end(&it[6]);
    dialog(&dlg, title, (const char*)0x004f6fd8, -2, it, 0);
    return run_dialog(&dlg, 0x140, 0xc8);
}
PORT_FN(0x0047a0d0, "UIDoYesNoCancelBox", UIDoYesNoCancelBox_n, fp_modal2)

// UIDoInputBox: a one-line text field on a copy of the text (0x578d70, at most `max`), Ctrl-V pastes; OK copies it
// back and gives true
static uint8_t __cdecl UIDoInputBox_n(const char* title, const char* prompt, char* text, int32_t max) {
    UIDialog dlg;
    UIDialogItem it[6];
    crt_strcpy((char*)(uintptr_t)S_INPUT_TEXT, text);
    UI_G32(S_INPUT_MAX) = max;
    item(&it[0], 4, 0, 0x16, 0, 0, 0, 0, 0, (void*)(uintptr_t)F_inputbox_paste, 0);
    item(&it[1], 5, 0, 0xa0, 0x28, 0, 0, prompt, 0, 0, 0xe);
    const int32_t cols = max < 0x20 ? max : 0x20;
    item(&it[2], 9, 0, 0x28, 0x50, cols << 3, 0x10, (const char*)1, max, (void*)(uintptr_t)S_INPUT_TEXT, 9);
    const char* ok = xlate(XL_OK);
    item(&it[3], 2, -2, 9, 0x99, 0, 0, ok, 0, 0, 0);
    const char* cancel = xlate(XL_CANCEL);
    item_ctor(&it[4], 2, -1, 0xd7, 0x99, 0, 0, cancel, 0, 0, 0, 0, 0);
    item_end(&it[5]);
    dialog(&dlg, title, (const char*)0x004f6fe4, 0, it, 0);
    if (run_dialog(&dlg, 0x140, 0xc8) != -2) return 0;
    crt_strcpy(text, (const char*)(uintptr_t)S_INPUT_TEXT);
    return 1;
}
static void fp_UIDoInputBox(Footprint& f, const char*, const char*, char*, int32_t) { f.replay_only = "a dialog: runs modally (its own input loop)"; }
PORT_FN(0x0047a3d0, "UIDoInputBox", UIDoInputBox_n, fp_UIDoInputBox)

// inputbox_paste: the clipboard's text into the input box's copy
static uint8_t __cdecl inputbox_paste_n(int32_t) {
    ccall<uint8_t>(F_ClipboardGetText, (char*)(uintptr_t)S_INPUT_TEXT, UI_G32(S_INPUT_MAX));
    return 0;
}
static void fp_inputbox_paste(Footprint& f, int32_t) { f.replay_only = "reads the clipboard"; }
PORT_FN(0x0047a6c0, "inputbox_paste", inputbox_paste_n, fp_inputbox_paste)

// the file boxes' frame, laid out as the original's (the list and name are left in statics while the box runs)
struct FileFrame {
    UIScrollAxis axis;          // +0 {64, 16, 0}
    UIDialog dlg;               // +0xc
    UIStringList list;          // +0x20 (64 entries of 32)
    UIDialogItem it[7];         // +0x34
    char found[0x104];          // +0x1bc
    char name[0x12c];           // +0x2c0
    char path[0x104];           // +0x3ec
};
static_assert(offsetof(FileFrame, list) == 0x20 && offsetof(FileFrame, it) == 0x34 && offsetof(FileFrame, name) == 0x2c0 &&
              sizeof(FileFrame) == 0x4f0, "FileFrame");
// the files matching dir + pattern, their names (no directory, no extension) into the list
static __forceinline void find_files(FileFrame& fr, uint32_t report_fmt, const char* report_arg) {
    void* h = ccall<void*>(F_FileFindFirst, (const char*)fr.path, (char*)fr.found, (int32_t)0x104);
    if (h == (void*)(intptr_t)-1) {
        UI_LogReport((const char*)(uintptr_t)report_fmt, report_arg);
        return;
    }
    do {
        char* dot = ccall<char*>(F_strrchr, (const char*)fr.found, (int)'.');
        if (dot) *(volatile char*)dot = 0;
        char* sl = ccall<char*>(F_strrchr, (const char*)fr.found, (int)'\\');
        const char* nm = sl ? sl + 1 : fr.found;
        tcall<void>(F_UIStringList_AddEntry, &fr.list, nm);
    } while (ccall<uint8_t>(F_FileFindNext, h, (char*)fr.found, (int32_t)0x104));
    ccall<void>(F_FileFindClose, h);
}
static __forceinline void file_setup(FileFrame& fr, const char* dir, const char* pattern, const char* fmt, const char* out) {
    UI_G32(S_FILE_SEL) = -1;
    UI_G32(S_FILE_LAST) = -1;
    UI_sprintf(fr.path, fmt, dir, pattern);
    crt_strcpy(fr.name, out);
    char* dot = ccall<char*>(F_strrchr, (const char*)fr.name, (int)'.');
    if (dot) *(volatile char*)dot = 0;
    tcall<void*>(F_UIStringList_ctor, &fr.list, (int32_t)0x40, (int32_t)0x20);
    UI_GP(void, S_FILE_LIST) = &fr.list;
    UI_GP(char, S_FILE_NAME) = fr.name;
    fr.axis.total = 0x40;
    fr.axis.visible = 0x10;
    fr.axis.pos = 0;
}

// UIDoOpenFileBox: a list of dir + pattern's files and the chosen name; OK: out = dir + name + pattern's extension,
// true if that file exists
static uint8_t __cdecl UIDoOpenFileBox_n(const char* title, const char* prompt, const char* pattern, char* out, int32_t,
                                         const char* dir) {
    FileFrame fr;
    file_setup(fr, dir, pattern, (const char*)0x004f6ff0, out);
    find_files(fr, 0x004f6ff8, pattern);
    item(&fr.it[0], 5, 0, 0xa0, 0x20, 0, 0, prompt, 0, 0, 0xe);
    item(&fr.it[1], 5, 0, 0x28, 0x22, 0, 0, fr.name, 0, 0, 0xc);
    item_ctor(&fr.it[2], 0x14, 0, 0x32, 0x42, 0xdc, 0x82, (const char*)&fr.axis, 0, &fr.list, 0, (int32_t*)(uintptr_t)S_FILE_SEL, 0);
    item(&fr.it[3], 0x11, 0, 0x121, 0x3c, 0xa, 0x87, (const char*)0x004e4db8, 0, &fr.axis, 0);
    const char* ok = xlate(XL_OK);
    item(&fr.it[4], 2, -2, 9, 0xcb, 0, 0, ok, 0, 0, 0);
    const char* cancel = xlate(XL_CANCEL);
    item_ctor(&fr.it[5], 2, -1, 0xd7, 0xcb, 0, 0, cancel, 0, 0, 0, 0, 0);
    item_end(&fr.it[6]);
    dialog(&fr.dlg, title, (const char*)0x004f7020, -2, fr.it, (UIIdle)(uintptr_t)F_file_idle);
    if (ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, (int32_t)0x140, (int32_t)0xfa, (int32_t)-999, (int32_t)-999, (int32_t)1) == -2) {
        UI_sprintf(out, (const char*)0x004f702c, dir, fr.name);
        const char* ext = ccall<const char*>(F_strrchr, pattern, (int)'.');
        if (ext) crt_strcat(out, ext);
        const uint8_t r = ccall<uint8_t>(F_file_exists, (const char*)out);
        tcall<void>(F_UIStringList_dtor, &fr.list);
        return r;
    }
    tcall<void>(F_UIStringList_dtor, &fr.list);
    return 0;
}
static void fp_file_box(Footprint& f, const char*, const char*, const char*, char*, int32_t, const char*) {
    f.replay_only = "a dialog: runs modally (its own input loop); finds files";
}
PORT_FN(0x0047a6e0, "UIDoOpenFileBox", UIDoOpenFileBox_n, fp_file_box)

// file_idle: a new list selection copied into the name field
static uint8_t __cdecl file_idle_n(int32_t*) {
    if (UI_G32(S_FILE_SEL) == UI_G32(S_FILE_LAST)) return 0;
    if (UI_G32(S_FILE_SEL) >= 0) {
        const char* e = tcall<const char*>(F_UIStringList_GetEntry, UI_GP(void, S_FILE_LIST), UI_G32(S_FILE_SEL));
        if (e) crt_strcpy(UI_GP(char, S_FILE_NAME), e);
        else UI_G32(S_FILE_SEL) = -1;
    }
    UI_G32(S_FILE_LAST) = UI_G32(S_FILE_SEL);
    return 0;
}
static void fp_file_idle(Footprint& f, int32_t*) {
    UI_FP(f, S_FILE_SEL, 4, "ui.obj file selection");
    UI_FP(f, S_FILE_LAST, 4, "ui.obj last file selection");
    const int32_t sel = UI_G32(S_FILE_SEL);
    const UIStringList* l = UI_GP(UIStringList, S_FILE_LIST);
    char* name = UI_GP(char, S_FILE_NAME);
    if (sel >= 0 && l && name && sel < l->count) {
        const char* e = l->entries[sel];
        if (e) f.add(name, (uint32_t)strlen(e) + 1, "the file box's name");
    }
}
PORT_FN(0x0047ab80, "file_idle", file_idle_n, fp_file_idle)

static uint8_t __cdecl file_exists_n(const char* name) {
    int32_t fd = ccall<int32_t>(F_FileOpen, name);
    if (!fd) return 0;
    ccall<void>(F_FileClose, &fd);
    return 1;
}
static void fp_file_exists(Footprint& f, const char*) { f.replay_only = "opens a file"; }
PORT_FN(0x0047abf0, "file_exists", file_exists_n, fp_file_exists)

// UIDoSaveFileBox: the same list, the name editable; OK: dir + name, confirmed if it exists, copied to out with the
// pattern's extension; true
static uint8_t __cdecl UIDoSaveFileBox_n(const char* title, const char* prompt, const char* pattern, char* out, int32_t max,
                                         const char* dir) {
    FileFrame fr;
    char full[0x200];
    file_setup(fr, dir, pattern, (const char*)0x004f7034, out);
    find_files(fr, 0x004f703c, fr.path);
    if (!(UI_G8(S_FILE_ONCE) & 1)) {
        UI_G8(S_FILE_ONCE) = UI_G8(S_FILE_ONCE) | 1;
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)XL_OVERWRITE_TITLE, (const char*)0x004f7064);
        ccall<int>(F_atexit, (uint32_t)F_file_atexit2);
    }
    if (!(UI_G8(S_FILE_ONCE) & 2)) {
        UI_G8(S_FILE_ONCE) = UI_G8(S_FILE_ONCE) | 2;
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)XL_OVERWRITE_PROMPT, (const char*)0x004f708c);
        ccall<int>(F_atexit, (uint32_t)F_file_atexit1);
    }
    item(&fr.it[0], 5, 0, 0xa0, 0x20, 0, 0, prompt, 0, 0, 0xe);
    const int32_t cols = max < 0x20 ? max : 0x20;
    item(&fr.it[1], 9, 0, 0x28, 0x24, cols << 3, 0x10, (const char*)1, max, fr.name, 9);
    item(&fr.it[2], 0x14, 0, 0x32, 0x42, 0xdc, 0x82, (const char*)&fr.axis, 0, &fr.list, 0, (int32_t*)(uintptr_t)S_FILE_SEL);
    item(&fr.it[3], 0x11, 0, 0x121, 0x3c, 0xa, 0x87, (const char*)0x004e4db8, 0, &fr.axis, 0);
    const char* ok = xlate(XL_OK);
    item(&fr.it[4], 2, -2, 9, 0xcb, 0, 0, ok, 0, 0, 0);
    const char* cancel = xlate(XL_CANCEL);
    item(&fr.it[5], 2, -1, 0xd7, 0xcb, 0, 0, cancel, 0, 0, 0);
    item_end(&fr.it[6]);
    dialog(&fr.dlg, title, (const char*)0x004f70c4, -2, fr.it, (UIIdle)(uintptr_t)F_file_idle);
    if (ccall<int32_t>(F_UIDoDialog, (const UIDialog*)&fr.dlg, (int32_t)0x140, (int32_t)0xfa, (int32_t)-999, (int32_t)-999, (int32_t)1) == -2) {
        UI_sprintf(full, (const char*)0x004f70d0, dir, fr.name);
        bool go = true;
        if (ccall<uint8_t>(F_file_exists, (const char*)full)) {
            const char* prompt2 = xlate(XL_OVERWRITE_PROMPT);
            const char* title2 = xlate(XL_OVERWRITE_TITLE);
            go = ccall<uint8_t>(F_UIDoOkCancelBox2, title2, prompt2) != 0;
        }
        if (go) {
            crt_strcpy(out, full);
            const char* ext = ccall<const char*>(F_strrchr, pattern, (int)'.');
            if (ext) crt_strcat(out, ext);
            tcall<void>(F_UIStringList_dtor, &fr.list);
            return 1;
        }
    }
    tcall<void>(F_UIStringList_dtor, &fr.list);
    return 0;
}
PORT_FN(0x0047ac30, "UIDoSaveFileBox", UIDoSaveFileBox_n, fp_file_box)

// =====================================================================================================================
// UIStringList, CreateMultiString
// =====================================================================================================================
static UIStringList* __fastcall UIStringList_ctor_n(UIStringList* self, Edx, int32_t cap, int32_t size) {
    self->capacity = cap;
    self->entry_size = size;
    self->count = 0;
    self->entries = (char**)ccall<void*>(F_MemAlloc, cap * 4);
    for (int32_t i = 0, n = cap; n > 0; n--, i++) {
        void* e = ccall<void*>(F_MemAlloc, size);
        self->entries[i] = (char*)e;
    }
    return self;
}
static void fp_UIStringList_ctor(Footprint& f, UIStringList*, Edx, int32_t, int32_t) { f.replay_only = "allocates the entries"; }
PORT_FN(0x0047b290, "UIStringList::UIStringList", UIStringList_ctor_n, fp_UIStringList_ctor)

static void __fastcall UIStringList_dtor_n(UIStringList* self, Edx) {
    for (int32_t i = 0; self->capacity > i;) {
        void* e = self->entries[i];
        i++;
        ccall<void>(F_Delete, e);
    }
    ccall<void>(F_Delete, (void*)self->entries);
}
static void fp_UIStringList_dtor(Footprint& f, UIStringList*, Edx) { f.replay_only = "frees the entries"; }
PORT_FN(0x0047b2e0, "UIStringList::~UIStringList", UIStringList_dtor_n, fp_UIStringList_dtor)

// AddEntry: strncpy into the next entry, terminated; nothing once it's full
static void __fastcall UIStringList_AddEntry_n(UIStringList* self, Edx, const char* s) {
    const int32_t n = self->count;
    if (!(self->capacity > n)) return;
    ccall<char*>(F_strncpy, self->entries[n], s, (uint32_t)self->entry_size);
    char** e = self->entries;
    const int32_t sz = self->entry_size;
    *(volatile char*)(e[self->count] + sz - 1) = 0;
    self->count = self->count + 1;
    self->changes = self->changes + 1;
}
static void fp_UIStringList_AddEntry(Footprint& f, UIStringList* self, Edx, const char*) {
    f.add(self, sizeof(UIStringList), "string list");
    const int32_t n = self->count;
    if (self->capacity > n && n >= 0 && self->entries && self->entry_size > 0 && self->entry_size < 0x100000)
        f.add(self->entries[n], (uint32_t)self->entry_size, "the entry");
}
PORT_FN(0x0047b320, "UIStringList::AddEntry", UIStringList_AddEntry_n, fp_UIStringList_AddEntry)

static char* __fastcall UIStringList_GetEntry_n(UIStringList* self, Edx, int32_t i) {
    if (i < 0) return 0;
    if (!(self->count > i)) return 0;
    return self->entries[i];
}
static void fp_UIStringList_GetEntry(Footprint&, UIStringList*, Edx, int32_t) {}
PORT_FN(0x0047b360, "UIStringList::GetEntry", UIStringList_GetEntry_n, fp_UIStringList_GetEntry)

// DeleteEntry (never called): the later entries copied down (strcpy), count down, changes up; a report for a bad index
static void __fastcall UIStringList_DeleteEntry_n(UIStringList* self, Edx, int32_t i) {
    if (i < 0 || i >= self->count) {
        UI_LogReport((const char*)0x004f70f4, i);
        return;
    }
    if (i + 1 < self->count) {
        do {
            char** e = self->entries + i;
            i++;
            crt_strcpy(e[0], e[1]);
        } while (i + 1 < self->count);
    }
    self->count = self->count - 1;
    self->changes = self->changes + 1;
}
static void fp_UIStringList_DeleteEntry(Footprint& f, UIStringList* self, Edx, int32_t i) {
    f.add(self, sizeof(UIStringList), "string list");
    if (i < 0 || !self->entries) return;
    for (int32_t k = i; k + 1 < self->count && k < 0x10000; k++) {
        const uint32_t n = (uint32_t)strlen(self->entries[k + 1]) + 1;
        f.add(self->entries[k], n, "an entry");
    }
}
PORT_FN(0x0047b380, "UIStringList::DeleteEntry", UIStringList_DeleteEntry_n, fp_UIStringList_DeleteEntry)

// CreateMultiString: the strings one after another, each with its terminator, and an empty one to end (Multi's form);
// the argument list ends with a null
static void __cdecl CreateMultiString_n(char* out, const char* s, Va va) {
    char* d = out;
    const char* p = s;
    int k = 0;
    for (;;) {
        crt_strcpy(d, p);
        d += crt_strlen(p) + 1;
        p = (const char*)(uintptr_t)((volatile uint32_t*)&va)[k++];
        if (!p) break;
    }
    *(volatile char*)d = 0;
}
static void fp_CreateMultiString(Footprint& f, char* out, const char* s, Va va) {
    uint32_t n = 0;
    const char* p = s;
    for (int k = 0; p && k <= 32;) {
        n += (uint32_t)strlen(p) + 1;
        p = k < 32 ? (const char*)(uintptr_t)va.w[k] : 0;
        k++;
    }
    f.add(out, n + 1, "the strings");
}
PORT_FN(0x0047b410, "CreateMultiString", CreateMultiString_n, fp_CreateMultiString)

#undef D
}  // namespace ui_dialog
}  // namespace
