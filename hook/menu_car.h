// menu_car.h -- M3 UI stage, step U2 (group A): the garage setup editor's layouts (library `menu`: mcar.obj), shared by
// hook/menu_car.cpp and test/world_menu_car.cpp.
//
// Recovered from the v1.0 disassembly (MenuEditCar's frame, the controls' Create / Added / Draw / destructors; out/types.tsv
// has the vtables and sizes). The controls are UICustomControls (ui_types.h): each adds its own widgets to the garage
// dialog when its CustomWidget is added, and draws its panel. Fields are volatile, as in ui_types.h: the rewrites read and
// write each one where the original does.
//
// Classes (vtable, size):
//   BalanceControl 0x4dee30 0x20       the balance summary (the dialog's right-hand panel)
//   ChassisControl 0x4dee80 0x28       springs / shocks / anti-roll, a page picked by radio button (1..6)
//   AlignControl 0x4deed0 0x34         camber / toe / height / brakes / lock, a page (1..9)
//   TorqueCurveControl 0x4ded40 0x1c   the engine's torque curve (DrivetrainControl's member at +0x20)
//   DrivetrainControl 0x4ded90 0x44    gears and final drive, a page (the gear being edited, 1..6)
//   AeroControl 0x4dede0 0x28          wings; two groups (adjustable / fixed kits) shown or hidden
//   FileControl 0x4def20 0x1c          the setup summary with Load / Save / Default (file_cb's own dialog)
//   UICustomControl 0x4db190 0x18      the base (root library)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace mcar {
using uit::UICustomControl;

struct McCtl {                                    // UICustomControl's fields (ui_types.h), as the controls derive them
    const void* volatile vtbl;                    // +0
    volatile int32_t x, y, w, h;                  // +4 the item's rectangle (_UIAddItems)
    void* volatile widget;                        // +0x14 its CustomWidget
};
static_assert(sizeof(McCtl) == 0x18, "McCtl");

struct BalanceControl : McCtl {                   // 0x20
    void* volatile stamp[2];                      // +0x18 "gbalance.stp"-style panel, +0x1c the car's outline
};
static_assert(sizeof(BalanceControl) == 0x20, "BalanceControl");

struct ChassisControl : McCtl {                   // 0x28
    void* volatile stamp[3];                      // +0x18 the graph panel, +0x1c / +0x20 the pictures
    volatile int32_t page;                        // +0x24 (1..6; Callback sets it from the radio buttons)
};
static_assert(sizeof(ChassisControl) == 0x28 && offsetof(ChassisControl, page) == 0x24, "ChassisControl");

struct AlignControl : McCtl {                     // 0x34
    void* volatile stamp[6];                      // +0x18 .. +0x2c
    volatile int32_t page;                        // +0x30 (1..9)
};
static_assert(sizeof(AlignControl) == 0x34 && offsetof(AlignControl, page) == 0x30, "AlignControl");

struct TorqueCurveControl : McCtl {               // 0x1c
    void* volatile stamp;                         // +0x18
};
static_assert(sizeof(TorqueCurveControl) == 0x1c, "TorqueCurveControl");

struct DrivetrainControl : McCtl {                // 0x44
    void* volatile stamp[2];                      // +0x18, +0x1c
    TorqueCurveControl torque;                    // +0x20 (its own vtable, stamp at +0x38)
    volatile int32_t page;                        // +0x3c the gear being edited (6 at first)
    volatile uint32_t group6;                     // +0x40 the sixth gear's group (EnterGroup's out; hidden for fewer gears)
};
static_assert(sizeof(DrivetrainControl) == 0x44 && offsetof(DrivetrainControl, page) == 0x3c, "DrivetrainControl");

struct AeroControl : McCtl {                      // 0x28
    void* volatile stamp[2];                      // +0x18, +0x1c
    volatile uint32_t group_fixed;                // +0x20 the group shown for a fixed kit (EnterGroup's out)
    volatile uint32_t group_adjust;               // +0x24 ... for an adjustable one
};
static_assert(sizeof(AeroControl) == 0x28, "AeroControl");

struct FileControl : McCtl {                      // 0x1c
    void* volatile stamp;                         // +0x18
};
static_assert(sizeof(FileControl) == 0x1c, "FileControl");

enum : uint32_t {
    VT_UICustomControl = 0x004db190, VT_TorqueCurveControl = 0x004ded40, VT_DrivetrainControl = 0x004ded90,
    VT_AeroControl = 0x004dede0, VT_BalanceControl = 0x004dee30, VT_ChassisControl = 0x004dee80,
    VT_AlignControl = 0x004deed0, VT_FileControl = 0x004def20,
};
// a control's size by its vtable (0 for anything else)
static inline uint32_t ctl_size(const void* c) {
    switch ((uint32_t)(uintptr_t)((const McCtl*)c)->vtbl) {
    case VT_UICustomControl: return 0x18;
    case VT_TorqueCurveControl: return 0x1c;
    case VT_DrivetrainControl: return 0x44;
    case VT_AeroControl: return 0x28;
    case VT_BalanceControl: return 0x20;
    case VT_ChassisControl: return 0x28;
    case VT_AlignControl: return 0x34;
    case VT_FileControl: return 0x1c;
    }
    return 0;
}

// ---- statics (v1.0) -------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // mcar.obj's own
    M_CAR_NAME = 0x004f9278,      // const char*: MenuEditCar's car (its first argument)
    M_CAR_DIR = 0x004f927c,       // const char*: ... its second (the directory the setups go in)
    M_TITLE = 0x004f9280,         // const char*: ... its fourth (the dialog's title), or 0
    M_ONE = 0x004f9284,           // int 1: the page radio buttons' variable
    M_SETUP = 0x0057af60,         // CarSetup (0xd4): the setup being edited
    M_SETUP_NAME = 0x0057aff4,    // ... its name (+0x94); '*' first: not saved since it changed
    M_SETUP_VER = 0x0057afec,     // ... +0x8c: 1
    M_SETUP_SIZE = 0x0057aff0,    // ... +0x90: 0xd4
    M_SETUP_ORIG = 0x0057b558,    // CarSetup: the setup as it was loaded (or last saved / loaded)
    M_SET = 0x0057a7e0,           // CarSetupSet (0x6ac): {version 1, size 0x6ac, slot, CarSetup[8]}
    M_SET_SLOT = 0x0057a7e8,      // int: the slot the setup was last saved to / loaded from
    M_SET_SETUPS = 0x0057a7ec,    // CarSetup[8]
    M_SET_NAMES = 0x0057a880,     // ... the first one's name
    M_SET_END = 0x0057af20,       // ... past the last one's name
    M_SLOT_F = 0x0057a77c,        // float: the slot
    M_CARFILE = 0x0057b038,       // CarFile (0x228): the car's .cf
    M_CARDATA = 0x0057b348,       // CarData (0x1e0): the car as set up (CarFileCombine)
    M_CARFILE_108 = 0x0057b140,   // CarFile +0x108, copied to 0x57a7c4 by MenuEditCar
    M_57A7C4 = 0x0057a7c4,
    M_CF_GEAR1 = 0x0057b120,      // float: the car file's first gear ratio (0: the setup's gears are edited)
    M_ONCE1 = 0x0057a720,         // MenuEditCar's Xlators built (bits 0..7)
    M_ONCE2 = 0x0057ae9c,         // ... (bits 0, 1)
    M_ONCE_FILE_CB = 0x0057b724,
    M_ONCE_SAVE = 0x0057b65c,
    M_ONCE_LOAD_CB = 0x0057b6dc,
    M_ONCE_LOAD = 0x0057b63c,
    M_ONCE_DEFAULT = 0x0057b264,
    M_ONCE_EXPORT1 = 0x0057b69c,
    M_ONCE_EXPORT2 = 0x0057b6bc,
    M_ONCE_BALANCE = 0x005d5b6c,
    M_ONCE_CHASSIS_DRAW = 0x005d5aec,
    M_ONCE_CHASSIS_ADDED = 0x005d5a8c,
    M_ONCE_ALIGN_DRAW = 0x005d5a6c,
    M_ONCE_ALIGN_ADDED = 0x005d59fc,
    M_ONCE_DRIVE_ADDED = 0x005d59ec,
    M_ONCE_AERO_ADDED = 0x005d598c,
    M_ONCE_FILE_DRAW = 0x005d592c,
    M_ONCE_FILE_ADDED = 0x005d58fc,
    // colours (set by the static initialisers)
    M_C_57A734 = 0x0057a734, M_C_57A7C8 = 0x0057a7c8, M_C_57AEDC = 0x0057aedc, M_C_57AF2C = 0x0057af2c,
    M_C_57B6AC = 0x0057b6ac, M_C_57A780 = 0x0057a780, M_C_57B6F0 = 0x0057b6f0,
};

// ---- the game's functions this object calls (v1.0 addresses) --------------------------------------------------------
enum : uint32_t {
    F_Win32GetUserDirectory = 0x00412cc0,
    F_FileCreate = 0x004115f0, F_FileSize = 0x00411890, F_FileReadExact = 0x004118b0, F_FileWrite = 0x00411a30,
    F_FileCreateDirectory = 0x00411d00,
    F_ResourceSetMustLoad = 0x00419a30, F_ResourceSetUnload = 0x00419bb0,
    F_TireBegin = 0x0043bd20, F_TireEnd = 0x0043bd30, F_Tire_GetForce = 0x0043bd40, F_TireCreate = 0x0043c080,
    F_TireDestroy = 0x0043c230, F_Damper_Setup = 0x00446a90, F_Damper_GetDampingRate = 0x00446ae0,
    F_CarFileLoad_file = 0x004647d0, F_CarFileCombine = 0x00464990, F_CarFileLoad_data = 0x00465100,
    F_CarFileLoadSetup = 0x00465320, F_CarFileSaveSetup = 0x004654f0, F_CarFileLoadDefaultSetup = 0x00465630,
    F_UIDoYesNoCancelBox = 0x0047a0d0,
    F_UICC_Dirty = 0x0047e820, F_UICC_AddNotification = 0x0047e840, F_UICC_AddItems = 0x0047e870,
    F_GraphDrawLinePlot = 0x004d9290, F_GraphDrawAxes = 0x004d9420,
    F_HTMLBegin = 0x004d99e0, F_HTMLEnd = 0x004d9a30, F_HTMLWriteLn = 0x004d9a60, F_HTMLWrite = 0x004d9ab0,
    // this object's own (called by address, as the original does, so a hooked rewrite is what runs)
    F_MenuEditCar = 0x004911a0, F_file_cb = 0x00491e70, F_load_setup_set = 0x004921b0, F_save_setup_set = 0x00492420,
    F_save_setup_cb = 0x00492550, F_save_setup_dlg = 0x00492560, F_load_setup_cb = 0x00492aa0,
    F_load_setup_dlg = 0x00492c10, F_default_setup_cb = 0x00493040, F_tire_fn = 0x00493160, F_shock_fn = 0x004931f0,
    F_export_setup = 0x004932a0, F_get_gear = 0x00493c40, F_power_fn = 0x00493c70,
};

}  // namespace mcar
