// ui_types.h -- M3 UI stage, step U1: the widget toolkit's layouts (library `ui`: ui.obj, _widget.obj, widget.obj,
// uistyle.obj), shared by hook/ui_*.cpp and test/world_ui.cpp.
//
// Recovered from the v1.0 disassembly (out/types.tsv has the vtables and the sizes MemAlloc is given; its field lists
// are partial). Every field is volatile: the rewrites read and write each one where the original does, in its order,
// with nothing merged or cached across a store that might alias (a notification copy, a text buffer, a pointer the
// caller aimed into the object). Sizes are the allocation sizes (_UIAddItems, UIDoMenu, UIDoDialog), static_assert'd.
//
// Classes (vtable, size):
//   Widget 0x4ddc88 0x228      the base: rectangle, flags, window, value notifications, group mask
//   StyleWidget 0x4ddce0 0x234 text drawn in a UIStyle: Button 0x4ddd40 0x290 (MenuButton 0x4ddb58 is a Button),
//     CheckBox 0x4dddf8 0x338, RadioButton 0x4ddeb0 0x340, StaticText 0x4de088 0x340, Numeric 0x4de140 0x294,
//     IntNumeric 0x4de1a0 0x294, Multi 0x4de200 0x2ac
//   BButton 0x4ddda0 0x238, BRadioButton 0x4ddf10 0x234, StampRoll 0x4dde58 0x230 (stamps)
//   ScrollButtonBase 0x4ddf68 0x238 (auto-repeat): ArrowButton 0x4ddfc8 0x24c, ScrollButton 0x4de028 0x240
//   LineWidget 0x4de0e8 0x23c, ListBox 0x4de260 0x24c (DropList 0x4de2b8 0x254), Input 0x4de310 0xa48 (CDetect
//     0x4de368 0xa68), Slider 0x4de3c0 0x254, ScrollBar 0x4de418 0x240
//   ui.obj's own: HotKey 0x4ddb00 0x234, CustomWidget 0x4ddbb8 0x22c (forwards to a UICustomControl), TitleBar
//     0x4ddc10 0x234; FStaticText (0x330) and Decal (0x22c) have a Draw and no vtable (never built)
//   WidgetWindow 0x464 (no vtable), UIStringList 0x14, UIScrollAxis 0xc, UIStyle 0x30, UIStyleDesc 0x34,
//   UIDialogItem 0x38, UIDialog 0x14, UIMenu 0x14
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "port.h"

namespace uit {

typedef int Edx;                                  // the unused edx of a __thiscall received as __fastcall
typedef uint8_t(__cdecl* UICallback)(int);        // a button's: nonzero -> WidgetExit(id)
typedef uint8_t(__cdecl* UIIdle)(int*);           // a dialog's idle function: nonzero -> WidgetExit(*code)

struct gxCanvas {                                 // gfx.obj (gx_2d.cpp)
    volatile int8_t format;
    volatile uint8_t flags;
    uint16_t _pad;
    uint8_t* volatile pixels;
    volatile int32_t w, h, pitch;
    volatile int32_t cx0, cy0, cx1, cy1;          // the clip rectangle (x1, y1 exclusive)
};
static_assert(sizeof(gxCanvas) == 0x24, "gxCanvas");

struct WidgetWindow;

// a value a widget watches: Widget::Update compares it with a private copy each frame and calls Callback(id, ptr)
struct Notification {                             // 0x10
    const void* volatile ptr;                     // +0
    volatile int32_t id;                          // +4
    volatile uint32_t size;                       // +8
    void* volatile copy;                          // +0xc, MemAlloc(size)
};

struct Widget {                                   // 0x228, vtable 0x4ddc88
    const void* volatile vtbl;                    // +0
    volatile int32_t x0, y0, x1, y1;              // +4 the rectangle, window coordinates (x1, y1 exclusive)
    volatile uint8_t over;                        // +0x14 the mouse is over it
    volatile uint8_t focus;                       // +0x15 it has the keyboard
    volatile uint8_t visible;                     // +0x16 (its group isn't hidden)
    volatile uint8_t enabled;                     // +0x17 (its group isn't disabled)
    WidgetWindow* volatile window;                // +0x18 set by WidgetWindow::AddWidget
    Notification notes[32];                       // +0x1c
    volatile int32_t nnotes;                      // +0x21c (no bound: the 33rd AddNotification writes over it)
    volatile uint8_t dirty;                       // +0x220 redraw
    uint8_t _pad221[3];
    volatile uint32_t groups;                     // +0x224 the window's group mask when it was added
};
static_assert(sizeof(Widget) == 0x228, "Widget");
static_assert(offsetof(Widget, nnotes) == 0x21c && offsetof(Widget, groups) == 0x224, "Widget fields");

struct StyleWidget : Widget {                     // 0x234, vtable 0x4ddce0
    volatile int32_t tx, ty;                      // +0x228 where the text is drawn
    volatile int32_t style;                       // +0x230 its UIStyle
};
static_assert(sizeof(StyleWidget) == 0x234, "StyleWidget");

struct Button : StyleWidget {                     // 0x290, vtable 0x4ddd40 (MenuButton 0x4ddb58)
    volatile uint8_t pressed;                     // +0x234
    volatile uint8_t b235;                        // +0x235 (0; never read)
    volatile uint8_t keys;                        // +0x236 space / return press it, the arrows tab
    volatile uint8_t repeat;                      // +0x237 fires every frame while held
    volatile UICallback cb;                       // +0x238
    volatile int32_t id;                          // +0x23c (-66666 0xfffefb96: a disabled label)
    char text[0x50];                              // +0x240
};
static_assert(sizeof(Button) == 0x290 && offsetof(Button, text) == 0x240, "Button");

struct BButton : Widget {                         // 0x238, vtable 0x4ddda0: a button drawn as a stamp
    volatile uint8_t pressed;                     // +0x228
    volatile uint8_t flash;                       // +0x229 drawn pressed once
    volatile uint8_t keys;                        // +0x22a
    volatile uint8_t repeat;                      // +0x22b
    volatile UICallback cb;                       // +0x22c
    volatile int32_t id;                          // +0x230
    void* volatile stamp;                         // +0x234
};
static_assert(sizeof(BButton) == 0x238, "BButton");

struct CheckBox : StyleWidget {                   // 0x338, vtable 0x4dddf8
    uint8_t* volatile value;                      // +0x234
    char text[0x100];                             // +0x238
};
static_assert(sizeof(CheckBox) == 0x338, "CheckBox");

struct StampRoll : Widget {                       // 0x230, vtable 0x4dde58: a stamp whose frame is *frame
    void* volatile stamp;                         // +0x228
    int32_t* volatile frame;                      // +0x22c
};
static_assert(sizeof(StampRoll) == 0x230, "StampRoll");

struct RadioButton : StyleWidget {                // 0x340, vtable 0x4ddeb0
    int32_t* volatile var;                        // +0x234
    volatile int32_t value;                       // +0x238 (group mode: the group mask it shows)
    volatile uint8_t groups_mode;                 // +0x23c shows its group while selected, hides it otherwise
    char text[0x103];                             // +0x23d
};
static_assert(sizeof(RadioButton) == 0x340 && offsetof(RadioButton, text) == 0x23d, "RadioButton");

struct BRadioButton : Widget {                    // 0x234, vtable 0x4ddf10
    int32_t* volatile var;                        // +0x228
    volatile int32_t value;                       // +0x22c
    void* volatile stamp;                         // +0x230
};
static_assert(sizeof(BRadioButton) == 0x234, "BRadioButton");

struct ScrollButtonBase : Widget {                // 0x238, vtable 0x4ddf68: Do() while held, faster and faster
    volatile uint8_t pressed;                     // +0x228
    volatile float timer;                         // +0x22c
    volatile float rate;                          // +0x230 (0.3 s at first)
    void* volatile stamp;                         // +0x234
};
static_assert(sizeof(ScrollButtonBase) == 0x238, "ScrollButtonBase");

struct ArrowButton : ScrollButtonBase {           // 0x24c, vtable 0x4ddfc8: steps a float
    float* volatile value;                        // +0x238
    volatile float lo, hi;                        // +0x23c, +0x240
    volatile int32_t steps;                       // +0x244
    volatile int32_t dir;                         // +0x248
};
static_assert(sizeof(ArrowButton) == 0x24c, "ArrowButton");

struct UIScrollAxis {                             // 0xc
    volatile int32_t total, visible, pos;
};

struct ScrollButton : ScrollButtonBase {          // 0x240, vtable 0x4de028: steps a scroll axis
    volatile int32_t dir;                         // +0x238 (1: down, else up)
    UIScrollAxis* volatile axis;                  // +0x23c
};
static_assert(sizeof(ScrollButton) == 0x240, "ScrollButton");

struct StaticText : StyleWidget {                 // 0x340, vtable 0x4de088
    const char* volatile src;                     // +0x234 watched: copied again when it changes
    uint32_t _238[2];
    char text[0x100];                             // +0x240
};
static_assert(sizeof(StaticText) == 0x340 && offsetof(StaticText, text) == 0x240, "StaticText");

struct LineWidget : Widget {                      // 0x23c, vtable 0x4de0e8: a filled rectangle
    volatile int32_t rx0, ry0, rx1, ry1;          // +0x228
    volatile uint32_t color;                      // +0x238
};
static_assert(sizeof(LineWidget) == 0x23c, "LineWidget");

struct FStaticText : Widget {                     // 0x330 (no vtable)
    const char* volatile fmt;                     // +0x228
    uint8_t _22c[0x100];
    void* volatile font;                          // +0x32c
};
static_assert(sizeof(FStaticText) == 0x330, "FStaticText");

struct Numeric : StyleWidget {                    // 0x294, vtable 0x4de140 (IntNumeric 0x4de1a0: an int value)
    volatile float scale, offset;                 // +0x234, +0x238
    const void* volatile value;                   // +0x23c float* / int*
    const char* volatile format;                  // +0x240
    char text[0x50];                              // +0x244
};
static_assert(sizeof(Numeric) == 0x294, "Numeric");

struct Multi : StyleWidget {                      // 0x2ac, vtable 0x4de200: cycles through strings
    int32_t* volatile var;                        // +0x234
    volatile int32_t count;                       // +0x238
    char text[0x50];                              // +0x23c
    const char* volatile options[8];              // +0x28c (no bound: a ninth string writes past the object)
};
static_assert(sizeof(Multi) == 0x2ac, "Multi");

struct UIStringList {                             // 0x14
    volatile int32_t capacity;                    // +0
    volatile int32_t entry_size;                  // +4
    volatile int32_t count;                       // +8
    volatile int32_t changes;                     // +0xc (watched by list boxes)
    char** volatile entries;                      // +0x10
};

struct ListBox : Widget {                         // 0x24c, vtable 0x4de260
    const UIStringList* volatile list;            // +0x228
    UIScrollAxis* volatile axis;                  // +0x22c
    volatile int32_t _230;
    volatile int32_t cols;                        // +0x234 width / 8
    volatile int32_t rows;                        // +0x238 height / row height
    volatile int32_t row_h;                       // +0x23c style 10's height + 2
    int32_t* volatile sel;                        // +0x240
    volatile int32_t scroll;                      // +0x244 -1 / 1 while dragging past an end
    volatile uint8_t dragging;                    // +0x248
};
static_assert(sizeof(ListBox) == 0x24c, "ListBox");

struct DropList : ListBox {                       // 0x254, vtable 0x4de2b8
    volatile int32_t open_w, open_h;              // +0x24c the list's size while open
};
static_assert(sizeof(DropList) == 0x254, "DropList");

struct Input : Widget {                           // 0xa48, vtable 0x4de310: a text field
    volatile int32_t _228;                        // +0x228 (0)
    volatile int32_t width;                       // +0x22c characters per line (|width|)
    volatile int32_t lines;                       // +0x230
    volatile int32_t cursor;                      // +0x234
    volatile int32_t line_start[0x200];           // +0x238 UpdateTable's (no bound)
    char* volatile buf;                           // +0xa38
    volatile int32_t style;                       // +0xa3c
    void* volatile caret;                         // +0xa40 the text cursor stamp
    void* volatile scroll;                        // +0xa44 the one-line field's frame stamp, or 0
};
static_assert(sizeof(Input) == 0xa48 && offsetof(Input, buf) == 0xa38, "Input");

struct CDetect : Input {                          // 0xa68, vtable 0x4de368: press a control to bind it
    char text[0x18];                              // +0xa48 the binding, as text (the Input's buffer)
    const char* volatile prompt;                  // +0xa60
    void* volatile control;                       // +0xa64 struct Control* (8 bytes)
};
static_assert(sizeof(CDetect) == 0xa68, "CDetect");

struct Slider : Widget {                          // 0x254, vtable 0x4de3c0
    volatile uint8_t dragging;                    // +0x228
    volatile float lo, hi;                        // +0x22c, +0x230
    float* volatile value;                        // +0x234
    volatile int32_t steps;                       // +0x238 |steps|
    volatile uint8_t neg;                         // +0x23c steps < 0
    void* volatile knob;                          // +0x240 stamp
    volatile int32_t tx, ty, tw, th;              // +0x244 the track
};
static_assert(sizeof(Slider) == 0x254, "Slider");

struct Decal : Widget {                           // 0x22c (no vtable)
    gxCanvas* volatile canvas;                    // +0x228
};

struct ScrollBar : Widget {                       // 0x240, vtable 0x4de418
    UIScrollAxis* volatile axis;                  // +0x228
    void* volatile stamp;                         // +0x22c
    volatile uint8_t dragging;                    // +0x230
    volatile int32_t type;                        // +0x234 0 vertical, else horizontal
    volatile int32_t grab_x, grab_y;              // +0x238 where the drag started, as a position
};
static_assert(sizeof(ScrollBar) == 0x240, "ScrollBar");

struct HotKey : Widget {                          // 0x234, vtable 0x4ddb00: an invisible key binding
    volatile uint16_t key;                        // +0x228
    uint16_t _22a;
    volatile UICallback cb;                       // +0x22c
    volatile int32_t id;                          // +0x230
};
static_assert(sizeof(HotKey) == 0x234, "HotKey");

struct UICustomControl {                          // 0x18, a menu's own control (its vtable: menus)
    const void* volatile vtbl;                    // +0 4 Create, 8 Destroy, 0xc Added, 0x10 Callback, 0x14 Update,
                                                  //    0x18 Draw, 0x1c Draw3D, 0x20.. mouse, 0x34 Over, 0x38 NotOver,
                                                  //    0x3c Focus, 0x40 UnFocus, 0x44 GetCursor, 0x48 CharHit,
                                                  //    0x4c IsTabStop
    volatile int32_t x, y, w, h;                  // +4 its item's (set by _UIAddItems)
    struct CustomWidget* volatile widget;         // +0x14
};

struct CustomWidget : Widget {                    // 0x22c, vtable 0x4ddbb8
    UICustomControl* volatile ctrl;               // +0x228
};
static_assert(sizeof(CustomWidget) == 0x22c, "CustomWidget");

struct TitleBar : Widget {                        // 0x234, vtable 0x4ddc10: drags the window
    volatile int32_t dx, dy;                      // +0x228 minus where it was grabbed
    volatile uint8_t dragging;                    // +0x230
};
static_assert(sizeof(TitleBar) == 0x234, "TitleBar");

struct WidgetWindow {                             // 0x464 (no vtable)
    void* volatile background;                    // +0 stamp, or 0 (cleared to the colour at 0x579704)
    gxCanvas canvas;                              // +4
    Widget* volatile widgets[256];                // +0x28 (no bound)
    volatile int32_t count;                       // +0x428
    void(__cdecl* volatile idle)();               // +0x42c
    Widget* volatile over;                        // +0x430
    Widget* volatile captured;                    // +0x434 the mouse went down on it
    Widget* volatile focus;                       // +0x438
    volatile int32_t x, y, w, h;                  // +0x43c
    volatile uint32_t next_group;                 // +0x44c the next group's bit (1, 2, 4, ...)
    volatile uint32_t group;                      // +0x450 the groups being entered (new widgets' mask)
    volatile uint32_t hidden;                     // +0x454
    volatile uint32_t disabled;                   // +0x458
    volatile uint8_t full_redraw;                 // +0x45c
    uint8_t _45d[3];
    volatile int32_t ui3d;                        // +0x460 (1: drawn over the 3D view, 2: 3D behind)
};
static_assert(sizeof(WidgetWindow) == 0x464 && offsetof(WidgetWindow, count) == 0x428, "WidgetWindow");

struct UIStyle {                                  // 0x30, the table at 0x5790e0 (32)
    volatile uint8_t used;                        // +0
    uint8_t _1[3];
    void* volatile font;                          // +4
    void* volatile pal[4];                        // +8 by state: 0 normal, 1 pressed, 2 over, 3 over+pressed
    volatile uint32_t flags;                      // +0x18 1/2/4 horizontal, 8/0x10/0x20/0x40 vertical
    volatile int32_t xo, yo;                      // +0x1c text offset
    volatile int32_t xo_down, yo_down;            // +0x24 ... pressed
    void* volatile stamp;                         // +0x2c
};
static_assert(sizeof(UIStyle) == 0x30, "UIStyle");

struct UIStyleDesc {                              // 0x34, the table at 0x4f6698 (24)
    const char* font;                             // +0
    uint32_t flags;                               // +4
    uint32_t c_normal, c_over, c_normal2;         // +8, +0xc, +0x10 the gradients' ends
    int32_t xo, yo;                               // +0x14
    uint32_t c_down, c_down_over, c_down2;        // +0x1c, +0x20, +0x24
    int32_t xo_down, yo_down;                     // +0x28
    const char* stamp;                            // +0x30
};
static_assert(sizeof(UIStyleDesc) == 0x34, "UIStyleDesc");

struct UIDialogItem {                             // 0x38 (replay.obj's constructor 0x4091c0 stores all 14 dwords)
    int32_t type;                                 // +0 1..0x17 (0 ends the list)
    int32_t id;                                   // +4
    int32_t x, y, w, h;                           // +8
    const char* text;                             // +0x18
    int32_t i1c;                                  // +0x1c
    void* data;                                   // +0x20
    int32_t style;                                // +0x24
    float lo, hi;                                 // +0x28
    int32_t* sel;                                 // +0x30
    const char* s34;                              // +0x34
};
static_assert(sizeof(UIDialogItem) == 0x38, "UIDialogItem");

struct UIDialog {                                 // 0x14
    const char* title;                            // +0
    const char* background;                       // +4
    int32_t default_id;                           // +8
    UIDialogItem* items;                          // +0xc
    UIIdle idle;                                  // +0x10
};

struct UIMenu {                                   // 0x14
    const char* title;                            // +0
    const char* background;                       // +4
    uint8_t back;                                 // +8 a "back" button
    int32_t default_id;                           // +0xc
    struct { const char* text; int32_t id; }* items;   // +0x10, ends with a null text
};

// ---- vtables and the classes' sizes ---------------------------------------------------------------------------------
enum : uint32_t {
    VT_HotKey = 0x004ddb00, VT_MenuButton = 0x004ddb58, VT_CustomWidget = 0x004ddbb8, VT_TitleBar = 0x004ddc10,
    VT_Widget = 0x004ddc88, VT_StyleWidget = 0x004ddce0, VT_Button = 0x004ddd40, VT_BButton = 0x004ddda0,
    VT_CheckBox = 0x004dddf8, VT_StampRoll = 0x004dde58, VT_RadioButton = 0x004ddeb0, VT_BRadioButton = 0x004ddf10,
    VT_ScrollButtonBase = 0x004ddf68, VT_ArrowButton = 0x004ddfc8, VT_ScrollButton = 0x004de028,
    VT_StaticText = 0x004de088, VT_LineWidget = 0x004de0e8, VT_Numeric = 0x004de140, VT_IntNumeric = 0x004de1a0,
    VT_Multi = 0x004de200, VT_ListBox = 0x004de260, VT_DropList = 0x004de2b8, VT_Input = 0x004de310,
    VT_CDetect = 0x004de368, VT_Slider = 0x004de3c0, VT_ScrollBar = 0x004de418,
};
// the size of the widget whose vtable this is (0x228, a plain Widget, for anything else)
static inline uint32_t widget_size(const void* w) {
    static const uint32_t k[][2] = {
        {VT_HotKey, 0x234}, {VT_MenuButton, 0x290}, {VT_CustomWidget, 0x22c}, {VT_TitleBar, 0x234},
        {VT_Widget, 0x228}, {VT_StyleWidget, 0x234}, {VT_Button, 0x290}, {VT_BButton, 0x238},
        {VT_CheckBox, 0x338}, {VT_StampRoll, 0x230}, {VT_RadioButton, 0x340}, {VT_BRadioButton, 0x234},
        {VT_ScrollButtonBase, 0x238}, {VT_ArrowButton, 0x24c}, {VT_ScrollButton, 0x240}, {VT_StaticText, 0x340},
        {VT_LineWidget, 0x23c}, {VT_Numeric, 0x294}, {VT_IntNumeric, 0x294}, {VT_Multi, 0x2ac},
        {VT_ListBox, 0x24c}, {VT_DropList, 0x254}, {VT_Input, 0xa48}, {VT_CDetect, 0xa68},
        {VT_Slider, 0x254}, {VT_ScrollBar, 0x240},
    };
    const uint32_t vt = (uint32_t)(uintptr_t)((const Widget*)w)->vtbl;
    for (auto& e : k)
        if (e[0] == vt) return e[1];
    return 0x228;
}

// ---- statics (v1.0) ---------------------------------------------------------------------------------------------------
enum : uint32_t {
    // ui.obj
    S_UI_SYNC = 0x004f6658,          // SingleBegin's handle
    S_STYLE_DESCS = 0x004f6698,      // UIStyleDesc[24], filled once by UIBegin
    S_STYLE_DESCS_END = 0x004f6b78,
    S_UI_BEGUN = 0x00578cec,         // bit 0: the style table is filled
    S_UI_LAST_TIME = 0x00578ce0,     // PTimeNow at the last _UIUpdateTime
    S_UI_DT = 0x00578ce8,            // float: seconds since, at most 0.25 (UIDeltaT)
    S_UI_STAMPS = 0x00578ed8,        // gxStamp*[4] (the scroll arrows)
    S_DIALOG_IDLE = 0x00578e80,      // the running dialog's idle function
    S_INPUT_TEXT = 0x00578d70,       // UIDoInputBox's buffer (0x100)
    S_INPUT_MAX = 0x00578e70,
    S_FILE_SEL = 0x00578e74,         // the file boxes' list selection
    S_FILE_LAST = 0x00578ce4,        // ... as file_idle last saw it
    S_FILE_LIST = 0x00578e78,        // UIStringList* (the box's own stack)
    S_FILE_NAME = 0x00578d48,        // char* (the box's own stack)
    S_FILE_ONCE = 0x00578e7c,        // bits 0, 1: the save box's Xlators are built
    S_END_ITEM = 0x00578d08,         // UIDialogItem: the end of a list (all 0)
    // _widget.obj
    S_CD_ONCE = 0x00578f0c,          // bit 0: CDetect's Xlator is built
    S_CD_TEXT = 0x00578f28,          // char[0x18]: the binding being detected
    S_CD_CONTROL = 0x00578f40,       // Control*
    S_C_TEXT = 0x00578f10, S_C_SLIDER = 0x00578f14, S_C_BAR_DARK = 0x00578f20, S_C_HILITE = 0x00578f24,
    S_C_CARET = 0x00579060, S_C_BAR_LIGHT = 0x00579064,
    // uistyle.obj
    S_STYLE_ZERO = 0x005790a8,       // UIStyle: an unused one (flags 9)
    S_STYLES = 0x005790e0,           // UIStyle[32]
    S_STYLES_END = 0x005796e0,
    // widget.obj
    S_MOUSE_X = 0x005796fc, S_MOUSE_Y = 0x00579700,
    S_CLEAR_COLOR = 0x00579704,
    S_EXIT = 0x00579724,             // u8: WidgetExit was called
    S_ACTIVE = 0x00579730,           // WidgetWindow*: the one running
    S_WINDOWS = 0x00579738,          // WidgetWindow*[16]
    S_WINDOWS_END = 0x00579778,
    S_EXIT_CODE = 0x00579784,
    S_CURSOR = 0x004f7228,           // gxStamp*: the widget under the mouse's cursor, or 0
    S_DEFAULT_CURSOR = 0x004f722c,
    // elsewhere
    S_GX_CANVAS = 0x004efbf8,        // gfx.obj: the current canvas
    S_SCREEN_W = 0x005228f4, S_SCREEN_H = 0x005228d4,   // gxScreenWid / gxScreenHit
    S_XLATOR_COOKIE = 0x004eb108,    // Xlator::g_cookie
    S_ADJUST_FDIV = 0x005024b8,
};

// ---- memory ------------------------------------------------------------------------------------------------------------
#define UI_G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define UI_G16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define UI_G32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define UI_GU32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define UI_GF(a) (*(volatile float*)(uintptr_t)(a))
#define UI_GP(T, a) (*(T* volatile*)(uintptr_t)(a))
static __forceinline UIStyle* ui_style_at(int32_t i) { return (UIStyle*)(uintptr_t)(S_STYLES + (uint32_t)i * 0x30u); }

// a virtual call: slot at byte `off` of obj's vtable, received as __thiscall (so a hooked rewrite is what runs)
template <typename R, typename... A> static __forceinline R vcall(const void* obj, uint32_t off, A... a) {
    typedef R(__fastcall * Fn)(const void*, Edx, A...);
    return ((Fn)(*(void* const* volatile*)obj)[off / 4])(obj, 0, a...);
}
// a __thiscall game function by address
template <typename R, typename... A> static __forceinline R tcall(uint32_t fn, const void* self, A... a) {
    typedef R(__fastcall * Fn)(const void*, Edx, A...);
    return ((Fn)(uintptr_t)fn)(self, 0, a...);
}
// a __cdecl game function by address
template <typename R, typename... A> static __forceinline R ccall(uint32_t fn, A... a) {
    typedef R(__cdecl * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}

// The compiler's inline string code, as the original runs it: strlen is `repne scasb`; strcpy copies strlen + 1
// bytes as dwords (`rep movsd`) then the 0..3 left (`rep movsb`), forward; a memcpy of n bytes the same (n / 4 dwords,
// n % 4 bytes), or byte by byte where the original uses `rep movsb` alone. Forward, one unit at a time, so a copy
// whose ends overlap does what the original's does.
static __forceinline uint32_t crt_strlen(const char* s) {
    const volatile char* p = s;
    uint32_t n = 0;
    while (p[n]) n++;
    return n;
}
static __forceinline void crt_copy(void* d, const void* s, uint32_t n) {        // rep movsd; rep movsb
    volatile uint32_t* dd = (volatile uint32_t*)d;
    const volatile uint32_t* sd = (const volatile uint32_t*)s;
    for (uint32_t i = n >> 2; i; i--) *dd++ = *sd++;
    volatile uint8_t* db = (volatile uint8_t*)dd;
    const volatile uint8_t* sb = (const volatile uint8_t*)sd;
    for (uint32_t i = n & 3; i; i--) *db++ = *sb++;
}
static __forceinline void crt_copy_bytes(void* d, const void* s, uint32_t n) {  // rep movsb
    volatile uint8_t* db = (volatile uint8_t*)d;
    const volatile uint8_t* sb = (const volatile uint8_t*)s;
    for (; n; n--) *db++ = *sb++;
}
static __forceinline void crt_strcpy(char* d, const char* s) { crt_copy(d, s, crt_strlen(s) + 1); }
// strcat: the end of d found first (repne scasb), then strlen(s) + 1 bytes as above
static __forceinline void crt_strcat(char* d, const char* s) {
    const uint32_t n = crt_strlen(s) + 1;
    crt_copy(d + crt_strlen(d), s, n);
}
// the inlined strcmp (two bytes a step): zero if equal
static __forceinline int crt_strcmp_ne(const char* a, const char* b) {
    const volatile uint8_t* p = (const volatile uint8_t*)a;
    const volatile uint8_t* q = (const volatile uint8_t*)b;
    for (;;) {
        const uint8_t c = *p;
        if (c != *q) return 1;
        if (!c) return 0;
        p++, q++;
    }
}
// `repe cmpsd; repe cmpsb` over n bytes: nonzero if they differ
static __forceinline int crt_memcmp_ne(const void* a, const void* b, uint32_t n) {
    const volatile uint8_t* p = (const volatile uint8_t*)a;
    const volatile uint8_t* q = (const volatile uint8_t*)b;
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != q[i]) return 1;
    return 0;
}
// cdq; xor eax, edx; sub eax, edx (INT_MIN stays INT_MIN)
static __forceinline int32_t iabs(int32_t v) {
    const uint32_t m = (uint32_t)(v >> 31);
    return (int32_t)(((uint32_t)v ^ m) - m);
}

// ---- the game's functions (v1.0 addresses) --------------------------------------------------------------------------
enum : uint32_t {
    // kernel
    F_LogReport = 0x00411150, F_LogPanic = 0x004112b0, F_FileOpen = 0x00411780, F_FileClose = 0x00411850,
    F_FileFindFirst = 0x00411c20, F_FileFindNext = 0x00411c80, F_FileFindClose = 0x00411cd0,
    F_PTimeNow = 0x00413b40, F_KeyClear = 0x00413c50, F_KeyGet = 0x00413c90, F_KeyHit = 0x00413cc0,
    F_MemAlloc = 0x004140e0, F_Delete = 0x00414390, F_MouseClear = 0x00414470, F_MouseGetEvent = 0x004144f0,
    F_MousePeek = 0x004145f0, F_SingleBegin = 0x00414f30, F_SingleEnd = 0x00415090,
    // useful
    F_LocaleConvertNumeric = 0x0041abe0, F_Xlator_ctor = 0x0041af80, F_Xlator_xlate = 0x0041afb0,
    F_ClipboardGetText = 0x004d98d0,
    // root
    F_UIDialogItem_ctor = 0x004091c0,
    // physics (controls)
    F_ControlUpdate = 0x0044b340, F_ControlDetectInit = 0x0044b4b0, F_ControlDetect = 0x0044b510,
    F_ControlToDisplayString = 0x0044ce40,
    // gx
    F_gxFlip = 0x0044e180, F_gxGrabScreen = 0x0044e1c0, F_gxReleaseScreen = 0x0044e210, F_mrBeginFrame = 0x0044e970,
    F_mrEndFrame = 0x0044e980, F_gxAllocCanvas = 0x0044f8c0, F_gxFreeCanvas = 0x0044f960, F_gxSetClip = 0x0044fdd0,
    F_gxRestoreClip = 0x0044fe10, F_gxSetCanvas = 0x0044fe30, F_gxClear = 0x0044ffd0, F_gxRect = 0x004506e0,
    F_gxPaste = 0x00451500, F_gxLine = 0x00452790, F_gxGetStamp = 0x00453500, F_gxForgetStamp = 0x004535d0,
    F_gxDrawStamp = 0x004535f0, F_gxStampHitTest = 0x00453950, F_gxStampWidth = 0x00453980,
    F_gxStampHeight = 0x00453990, F_gxStampCount = 0x004539a0, F_gxStampHotSpot = 0x004539b0,
    F_gxPaletteCreate = 0x00453a00, F_gxPaletteDestroy = 0x00453a10, F_gxPaletteMakeGradient = 0x00453a60,
    F_gxText = 0x00453bd0, F_gxTextHeight = 0x00453ca0, F_gxFontGet = 0x00453f30, F_gxFontForget = 0x00454020,
    F_gxFontStringWidth = 0x00454040, F_gxFontStringWidthN = 0x00454080, F_gxFontAscent = 0x004540f0,
    F_gxFontDescent = 0x00454130, F_gxFontHeight = 0x00454180, F_gxFontPrintf = 0x004541a0,
    F_gxFontPrintN = 0x00454320,
    // the game's C runtime
    F_strchr = 0x004ce0c0, F_atexit = 0x004ceff0, F_sprintf = 0x004cf0a0, F_strrchr = 0x004cf130,
    F_strncpy = 0x004cf3a0, F_memmove = 0x004cf400, F_tolower = 0x004cfd50,
    // this library (called by address, as the original does, so a hooked rewrite is what runs)
    F_UIStyleBegin = 0x0047f330, F_UIStyleEnd = 0x0047f370, F_UIAddStyle = 0x0047f3a0, F_UIRemoveStyle = 0x0047f4e0,
    F_UIStyleAssert = 0x0047f560, F_UIStyleGetBounds = 0x0047f570, F_get_style = 0x0047f7d0,
    F_charcount = 0x0047f7e0, F_UIStylePointToIndex = 0x0047f800, F_UIStyleIndexToPoint = 0x0047f880,
    F_UIStyleWidth = 0x0047f910, F_UIStyleHeight = 0x0047f9b0, F_UIStyleDraw = 0x0047fa40,
    F_UIStyleWordWrap = 0x0047fb90, F_get_width = 0x0047fc20,
    F_WidgetBegin = 0x0047fe50, F_WidgetEnd = 0x0047fe90, F_WidgetCreateWindow = 0x0047fea0,
    F_WidgetDestroyWindow = 0x0047ff10, F_WidgetExit = 0x0047ff60, F_WidgetEnterGroup = 0x0047ff80,
    F_WidgetLeaveGroup = 0x0047ff90, F_WidgetHideGroup = 0x0047ffa0, F_WidgetShowGroup = 0x0047ffb0,
    F_WidgetEnableGroup = 0x0047ffc0, F_WidgetDisableGroup = 0x0047ffd0, F_WidgetGetActiveWindow = 0x0047ffe0,
    F_WidgetAddItem = 0x0047fff0, F_WidgetSetIdleFunction = 0x00480000, F_WidgetUpdate = 0x00480010,
    F_WidgetExecuteWindow = 0x00480260, F_WidgetSetDefault = 0x00480330, F_WidgetWindow_ctor = 0x00480340,
    F_WidgetWindow_dtor = 0x004803f0, F_WW_Activate = 0x00480440, F_WW_Deactivate = 0x00480470,
    F_WW_MouseDown = 0x004804b0, F_WW_MouseUp = 0x00480520, F_WW_MouseRDown = 0x00480560,
    F_WW_MouseRUp = 0x004805d0, F_WW_MouseMove = 0x00480610, F_WW_CharHit = 0x00480690,
    F_WW_NextWidget = 0x00480700, F_WW_PrevWidget = 0x00480760, F_WW_set_focus = 0x004807a0,
    F_WW_find_widget = 0x004807e0, F_WW_find_widget_index = 0x00480830, F_WW_Update = 0x00480860,
    F_WW_Draw = 0x004808d0, F_WW_Draw3D = 0x00480ae0, F_WW_EnterGroup = 0x00480b20, F_WW_Assert = 0x00480b60,
    F_WW_LeaveGroup = 0x00480b70, F_WW_HideGroup = 0x00480b90, F_WW_ShowGroup = 0x00480bf0,
    F_WW_EnableGroup = 0x00480c50, F_WW_DisableGroup = 0x00480cc0, F_WW_AddWidget = 0x00480d20,
    F_WW_SetDefault = 0x00480d70, F_Widget_TabNext = 0x00480dd0, F_Widget_TabOff = 0x00480de0,
    F_Widget_TabPrev = 0x00480df0,
    F_Widget_ctor = 0x0047bc30, F_Widget_dtor = 0x0047bc80, F_Widget_Move = 0x0047bcc0, F_Widget_Resize = 0x0047bcf0,
    F_Widget_Draw = 0x0047bd10, F_Widget_CharHit = 0x0047bd20, F_Widget_IsDirty = 0x0047bd30,
    F_Widget_Update = 0x0047bd50, F_Widget_AddNotification = 0x0047bdc0, F_Widget_RemoveNotification = 0x0047be30,
    F_StyleWidget_ctor = 0x0047beb0, F_StyleWidget_UpdateText = 0x0047bf40, F_Button_ctor = 0x0047c000,
    F_BButton_ctor = 0x0047c140, F_CheckBox_ctor = 0x0047c390, F_StampRoll_ctor = 0x0047c410,
    F_StampRoll_dtor = 0x0047c4e0, F_RadioButton_ctor = 0x0047c510, F_BRadioButton_ctor = 0x0047c5f0,
    F_ScrollButtonBase_ctor = 0x0047c700, F_ArrowButton_ctor = 0x0047c8a0, F_ScrollButton_ctor = 0x0047c9c0,
    F_StaticText_ctor = 0x0047ca60, F_LineWidget_ctor = 0x0047cb40, F_Numeric_ctor = 0x0047cc40,
    F_Numeric_update_text = 0x0047ccb0, F_IntNumeric_ctor = 0x0047cd20, F_IntNumeric_update_text = 0x0047cd90,
    F_Multi_ctor = 0x0047ce00, F_Multi_update_text = 0x0047cec0, F_ListBox_ctor = 0x0047cf30,
    F_ListBox_MouseMove = 0x0047d010, F_ListBox_Draw = 0x0047d130, F_DropList_ctor = 0x0047d220,
    F_Input_ctor = 0x0047d3a0, F_Input_dtor = 0x0047d480, F_Input_UpdateTable = 0x0047d670,
    F_Input_GetLine = 0x0047d790, F_CDetect_ctor = 0x0047db50, F_cdetect_idle = 0x0047dcc0,
    F_Slider_ctor = 0x0047dd20, F_Slider_is_hot_area = 0x0047df60, F_ScrollBar_ctor = 0x0047e1d0,
    F_UIBegin = 0x004779a0, F_UIEnd = 0x00478380, F_UIDoMenu = 0x004783d0, F_UIUpdateTime = 0x004785a0,
    F_UIAddItems = 0x004785f0, F_UIDoDialog = 0x00478ff0, F_dialog_idle_func = 0x00479170,
    F_UIHideGroup = 0x004791b0, F_UIShowGroup = 0x004791e0, F_UIDeltaT = 0x00479270, F_UIDoOkCancelBox2 = 0x00479740,
    F_UIDoOkCancelBox3 = 0x00479760, F_inputbox_paste = 0x0047a6c0, F_file_idle = 0x0047ab80,
    F_file_exists = 0x0047abf0, F_UIStringList_ctor = 0x0047b290, F_UIStringList_dtor = 0x0047b2e0,
    F_UIStringList_AddEntry = 0x0047b320, F_UIStringList_GetEntry = 0x0047b360,
    F_file_atexit1 = 0x0047b270, F_file_atexit2 = 0x0047b280, F_cdetect_atexit = 0x0047dd10,
};
// the few game functions called through a typed pointer (variadic, float arguments)
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
typedef void(__cdecl* Log_t)(const char*, ...);
typedef void(__cdecl* FontPrintf_t)(void*, void*, uint32_t, int32_t, int32_t, const char*, ...);
typedef void(__cdecl* Assert_t)(int, const char*, ...);
#define UI_LogReport ((Log_t)(uintptr_t)F_LogReport)
#define UI_LogPanic ((Log_t)(uintptr_t)F_LogPanic)
#define UI_sprintf ((Sprintf_t)(uintptr_t)F_sprintf)

// Xlator (locale.obj, krn_res.cpp): {key, value, cookie}; `value` is fresh while cookie == Xlator::g_cookie
static __forceinline const char* xlate(uint32_t xl) {
    if (UI_GU32(xl + 8) != UI_GU32(S_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (void*)(uintptr_t)xl);
    return UI_GP(const char, xl + 4);
}

// ---- footprints --------------------------------------------------------------------------------------------------------
#define UI_FP(f, addr, n, what) (f).add((void*)(uintptr_t)(addr), (uint32_t)(n), (what))
// a widget, sized by its class
#define UI_FP_WIDGET(f, w, what) do { if (w) (f).add((void*)(w), widget_size(w), (what)); } while (0)
// the bytes of a canvas's rows, the whole canvas: what a draw clipped to it can write (a 32-bit canvas as 4 bytes a
// pixel), and the canvas itself (its clip rectangle)
#define UI_FP_CANVAS(f, c) do {                                                                              \
        const gxCanvas* c_ = (const gxCanvas*)(c);                                                          \
        if (c_) {                                                                                            \
            (f).add((void*)c_, sizeof(gxCanvas), "canvas");                                                  \
            const int64_t n_ = (int64_t)(c_->h > 0 ? c_->h - 1 : 0) * c_->pitch + (int64_t)(c_->w > 0 ? c_->w : 0) * 4; \
            if (c_->pixels && c_->pitch > 0 && n_ > 0 && n_ <= 0x4000000) (f).add(c_->pixels, (uint32_t)n_, "canvas pixels"); \
        }                                                                                                    \
    } while (0)
// the current canvas (gfx.obj) and what it holds
#define UI_FP_CUR_CANVAS(f) do { UI_FP(f, S_GX_CANVAS, 4, "gfx.obj current canvas");                        \
        UI_FP_CANVAS(f, UI_GP(gxCanvas, S_GX_CANVAS)); } while (0)

// What a widget's handlers can reach. Most write only their widget and the values it edits (a check box's byte, a
// list's selection and scroll axis, a slider's float, a text field's buffer), the copies its notifications keep, and
// WidgetExit's two statics; a few reach the running window (RadioButton's groups, the buttons' Tab keys, TitleBar's
// drag). Some can run code nobody can bound: a CustomWidget forwards everything to a UICustomControl (menu code), a
// button with a callback calls it, CDetect::MouseUp runs a modal dialog, and a widget of a class this library doesn't
// know could do anything -- a check that reaches one of those is left to the session replays (replay_only).
static const char* ui_unbounded(const Widget* w) {
    if (!w) return 0;
    switch ((uint32_t)(uintptr_t)w->vtbl) {
    case VT_CustomWidget: return "a CustomWidget forwards to its UICustomControl (menu code)";
    case VT_CDetect: return "CDetect::MouseUp runs a modal dialog";
    case VT_Button: case VT_MenuButton: return ((const Button*)w)->cb ? "calls a button's callback" : 0;
    case VT_BButton: return ((const BButton*)w)->cb ? "calls a button's callback" : 0;
    case VT_HotKey: return ((const HotKey*)w)->cb ? "calls a hot key's callback" : 0;
    case VT_Widget: case VT_StyleWidget: case VT_TitleBar: case VT_CheckBox: case VT_StampRoll: case VT_RadioButton:
    case VT_BRadioButton: case VT_ScrollButtonBase: case VT_ArrowButton: case VT_ScrollButton: case VT_StaticText:
    case VT_LineWidget: case VT_Numeric: case VT_IntNumeric: case VT_Multi: case VT_ListBox: case VT_DropList:
    case VT_Input: case VT_Slider: case VT_ScrollBar:
        return 0;
    }
    return "a widget of a class outside the toolkit";
}
static inline int32_t ui_clamp_count(int32_t n, int32_t max) { return n < 0 ? 0 : n > max ? max : n; }
static void ui_fp_window_objects(Footprint& f, const WidgetWindow* win);
// w, and what its handlers write (see above); false (and the footprint replay_only) if one is unbounded
static bool ui_fp_reach(Footprint& f, const Widget* w) {
    if (!w) return true;
    if (const char* why = ui_unbounded(w)) { f.replay_only = why; return false; }
    UI_FP_WIDGET(f, w, "widget");
    const int32_t nn = ui_clamp_count(w->nnotes, 32);
    for (int32_t i = 0; i < nn; i++)
        if (w->notes[i].copy && w->notes[i].size && w->notes[i].size < 0x100000)
            f.add(w->notes[i].copy, w->notes[i].size, "a notification's copy");
    UI_FP(f, S_EXIT, 1, "widget.obj exit flag");
    UI_FP(f, S_EXIT_CODE, 4, "widget.obj exit code");
    switch ((uint32_t)(uintptr_t)w->vtbl) {
    case VT_CheckBox: if (((const CheckBox*)w)->value) f.add(((const CheckBox*)w)->value, 1, "check box value"); break;
    case VT_RadioButton: {
        const RadioButton* r = (const RadioButton*)w;
        if (r->var) f.add(r->var, 4, "radio button variable");
        if (r->groups_mode) ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
        break;
    }
    case VT_BRadioButton: if (((const BRadioButton*)w)->var) f.add(((const BRadioButton*)w)->var, 4, "radio button variable"); break;
    case VT_Multi: if (((const Multi*)w)->var) f.add(((const Multi*)w)->var, 4, "multi variable"); break;
    case VT_ListBox: case VT_DropList: {
        const ListBox* l = (const ListBox*)w;
        if (l->sel) f.add(l->sel, 4, "list selection");
        if (l->axis) f.add(l->axis, sizeof(UIScrollAxis), "list scroll axis");
        break;
    }
    case VT_ArrowButton: if (((const ArrowButton*)w)->value) f.add(((const ArrowButton*)w)->value, 4, "arrow button value"); break;
    case VT_ScrollButton: if (((const ScrollButton*)w)->axis) f.add(((const ScrollButton*)w)->axis, sizeof(UIScrollAxis), "scroll axis"); break;
    case VT_Slider: if (((const Slider*)w)->value) f.add(((const Slider*)w)->value, 4, "slider value"); break;
    case VT_ScrollBar: if (((const ScrollBar*)w)->axis) f.add(((const ScrollBar*)w)->axis, sizeof(UIScrollAxis), "scroll bar axis"); break;
    case VT_Input: {
        const Input* in = (const Input*)w;
        const uint32_t n = (uint32_t)in->width * (uint32_t)in->lines;
        if (in->buf && n && n < 0x100000) f.add(in->buf, n, "text field buffer");
        break;
    }
    case VT_TitleBar: ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE)); break;
    case VT_Button: case VT_MenuButton:
        if (((const Button*)w)->keys) ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
        break;
    case VT_BButton:
        if (((const BButton*)w)->keys) ui_fp_window_objects(f, UI_GP(WidgetWindow, S_ACTIVE));
        break;
    }
    return true;
}
// a window and its widgets' objects (a group shown or hidden, the focus moved)
static void ui_fp_window_objects(Footprint& f, const WidgetWindow* win) {
    if (!win) return;
    f.add((void*)win, sizeof(WidgetWindow), "window");
    const int32_t n = ui_clamp_count(win->count, 256);
    for (int32_t i = 0; i < n; i++) UI_FP_WIDGET(f, win->widgets[i], "a window's widget");
}
// a window, and what all its widgets' handlers reach; false (replay_only) if one is unbounded
static bool ui_fp_window_reach(Footprint& f, const WidgetWindow* win) {
    if (!win) return true;
    f.add((void*)win, sizeof(WidgetWindow), "window");
    const int32_t n = ui_clamp_count(win->count, 256);
    for (int32_t i = 0; i < n; i++)
        if (!ui_fp_reach(f, win->widgets[i])) return false;
    return true;
}
// the focus moving in a window (IsTabStop, Focus, UnFocus of its widgets, GetID, HitTest, Draw3D): only a CustomWidget
// or a class outside the toolkit does more than set its own fields
static bool ui_fp_window_focus(Footprint& f, const WidgetWindow* win) {
    if (!win) return true;
    const int32_t n = ui_clamp_count(win->count, 256);
    for (int32_t i = 0; i < n; i++) {
        const Widget* w = win->widgets[i];
        if (!w) continue;
        const uint32_t vt = (uint32_t)(uintptr_t)w->vtbl;
        if (vt == VT_CustomWidget) { f.replay_only = "a CustomWidget's IsTabStop / Focus / UnFocus / Draw (menu code)"; return false; }
        if (widget_size(w) == 0x228 && vt != VT_Widget) { f.replay_only = "a widget of a class outside the toolkit"; return false; }
    }
    ui_fp_window_objects(f, win);
    return true;
}
static bool ui_fp_active_focus(Footprint& f) { return ui_fp_window_focus(f, UI_GP(WidgetWindow, S_ACTIVE)); }

}  // namespace uit
