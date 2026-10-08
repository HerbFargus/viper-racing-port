// viperport -- milestone M2, stage 1: the game's window and input on SDL2.
//
// The game reaches the platform through a narrow layer (kernel:win32/scan/key/mouse/joy.obj). This file
// replaces its window and input half, function by function, leaving everything above untouched:
//
//   create_window    an SDL window (borderless, screen-sized, on top -- as before); its HWND goes into
//                    the game's window global, so DirectDraw still takes it over for fullscreen drawing
//   Win32Idle        the SDL event pump: keys and characters into the game's key queue, mouse events
//                    into its mouse queue, focus changes into its active flag (and the same blocking
//                    wait while inactive), quit -> exit; raw messages still reach the game's message
//                    hooks (winsock's), through SDL's Windows message hook
//   ScanBegin/Update the 256-byte DirectInput keyboard state the driving controls read, from SDL's
//                    keyboard state
//   Joy*             SDL game controllers and joysticks for DirectInput's, and SDL haptics for its
//                    constant-force effect, with the game's own force maths
//
// Off unless viperport.ini (next to the DLL) has `[platform] sdl=1`. SDL2.dll is delay-loaded, so the DLL
// still loads without it when the switch is off.
//
// A session (session.cpp) records what this layer hands the game -- each Win32Idle's events as "ops", the keys held,
// the joystick -- and a session replay hands the game the recording's instead: the player's input is ignored (closing
// the window still quits), and switching away really doesn't change the game (the recording's switches are applied).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "vp_os.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "SDL.h"
#include "SDL_syswm.h"
#include "viperport.h"
#include "session.h"
#include "standalone.h"
#ifndef _WIN32
#include "w32_user.h"                                            // (R2b: the game's window among the stand-ins')
#include "w32_net.h"                                             // (R2b: winsock's posted replies)
#endif

namespace {

// ---- the game's side, by v1.0 address (translated per build through A()) -----------------------
typedef void(__cdecl* U32_t)(unsigned);
typedef void(__cdecl* U32U8_t)(unsigned, unsigned char);
typedef void(__cdecl* Void_t)(void);
typedef void(__cdecl* MouseQueue_t)(int type, int x, int y, int state);
typedef unsigned char(__cdecl* MsgHook_t)(unsigned msg, int wparam, int lparam);
typedef unsigned char(__cdecl* U8_t)(void);

U32_t KeyDown, KeyUp;
U32U8_t KeyQueueChar, KeyQueueMetaChar;
Void_t KeyClearBits, gxRestore;
Void_t PhysicsPause, PhysicsUnpause;                             // the race stands still while switched away (FIX)
U8_t PhysicsIsPaused, MultiEnabled;
uint32_t g_phys_state;                                           // physics.obj's task state: 3 while a race runs
bool g_away_paused;                                              // we paused the race; we unpause it
MouseQueue_t MouseQueueEvent;
struct {
    uint32_t hwnd, prev_foreground, inactive, hooks, title, class_name;   // win32.obj
    uint32_t dik, shift, ctrl, alt;                              // scan.obj: DirectInput key state
} G;

// ---- SDL side ----------------------------------------------------------------------------------------
SDL_Window* g_window;
HWND g_hwnd;
bool g_gl;                                                       // viperport.ini renderer=gl: an OpenGL window
struct { int x0, y0, w, h, game_w, game_h; bool set; } g_view;   // where the renderer shows the game's picture
Uint32 g_mouse_buttons;
volatile LONG g_n_keys, g_n_motion, g_n_clicks, g_n_activations;   // for the exit log: did input arrive?                                          // the game's MK-style mask: 1 left, 2 right, 4 middle
SDL_Joystick* g_joy;
SDL_GameController* g_pad;
SDL_Haptic* g_haptic;
int g_effect = -1;
bool g_ff_on;
float g_ff_prev_b, g_ff_prev_c;                                  // JoySetForce's remembered inputs
char g_joy_name[128];

// SDL scancode -> DirectInput key code (set-1 scancode, +0x80 when extended) and Windows virtual key
struct Key { SDL_Scancode sc; uint8_t dik; uint8_t vk; };
const Key KEYS[] = {
    {SDL_SCANCODE_ESCAPE, 0x01, 0x1b}, {SDL_SCANCODE_1, 0x02, '1'}, {SDL_SCANCODE_2, 0x03, '2'},
    {SDL_SCANCODE_3, 0x04, '3'}, {SDL_SCANCODE_4, 0x05, '4'}, {SDL_SCANCODE_5, 0x06, '5'},
    {SDL_SCANCODE_6, 0x07, '6'}, {SDL_SCANCODE_7, 0x08, '7'}, {SDL_SCANCODE_8, 0x09, '8'},
    {SDL_SCANCODE_9, 0x0a, '9'}, {SDL_SCANCODE_0, 0x0b, '0'}, {SDL_SCANCODE_MINUS, 0x0c, 0xbd},
    {SDL_SCANCODE_EQUALS, 0x0d, 0xbb}, {SDL_SCANCODE_BACKSPACE, 0x0e, 0x08}, {SDL_SCANCODE_TAB, 0x0f, 0x09},
    {SDL_SCANCODE_Q, 0x10, 'Q'}, {SDL_SCANCODE_W, 0x11, 'W'}, {SDL_SCANCODE_E, 0x12, 'E'},
    {SDL_SCANCODE_R, 0x13, 'R'}, {SDL_SCANCODE_T, 0x14, 'T'}, {SDL_SCANCODE_Y, 0x15, 'Y'},
    {SDL_SCANCODE_U, 0x16, 'U'}, {SDL_SCANCODE_I, 0x17, 'I'}, {SDL_SCANCODE_O, 0x18, 'O'},
    {SDL_SCANCODE_P, 0x19, 'P'}, {SDL_SCANCODE_LEFTBRACKET, 0x1a, 0xdb}, {SDL_SCANCODE_RIGHTBRACKET, 0x1b, 0xdd},
    {SDL_SCANCODE_RETURN, 0x1c, 0x0d}, {SDL_SCANCODE_LCTRL, 0x1d, 0x11}, {SDL_SCANCODE_A, 0x1e, 'A'},
    {SDL_SCANCODE_S, 0x1f, 'S'}, {SDL_SCANCODE_D, 0x20, 'D'}, {SDL_SCANCODE_F, 0x21, 'F'},
    {SDL_SCANCODE_G, 0x22, 'G'}, {SDL_SCANCODE_H, 0x23, 'H'}, {SDL_SCANCODE_J, 0x24, 'J'},
    {SDL_SCANCODE_K, 0x25, 'K'}, {SDL_SCANCODE_L, 0x26, 'L'}, {SDL_SCANCODE_SEMICOLON, 0x27, 0xba},
    {SDL_SCANCODE_APOSTROPHE, 0x28, 0xde}, {SDL_SCANCODE_GRAVE, 0x29, 0xc0}, {SDL_SCANCODE_LSHIFT, 0x2a, 0x10},
    {SDL_SCANCODE_BACKSLASH, 0x2b, 0xdc}, {SDL_SCANCODE_Z, 0x2c, 'Z'}, {SDL_SCANCODE_X, 0x2d, 'X'},
    {SDL_SCANCODE_C, 0x2e, 'C'}, {SDL_SCANCODE_V, 0x2f, 'V'}, {SDL_SCANCODE_B, 0x30, 'B'},
    {SDL_SCANCODE_N, 0x31, 'N'}, {SDL_SCANCODE_M, 0x32, 'M'}, {SDL_SCANCODE_COMMA, 0x33, 0xbc},
    {SDL_SCANCODE_PERIOD, 0x34, 0xbe}, {SDL_SCANCODE_SLASH, 0x35, 0xbf}, {SDL_SCANCODE_RSHIFT, 0x36, 0x10},
    {SDL_SCANCODE_KP_MULTIPLY, 0x37, 0x6a}, {SDL_SCANCODE_LALT, 0x38, 0x12}, {SDL_SCANCODE_SPACE, 0x39, 0x20},
    {SDL_SCANCODE_CAPSLOCK, 0x3a, 0x14}, {SDL_SCANCODE_F1, 0x3b, 0x70}, {SDL_SCANCODE_F2, 0x3c, 0x71},
    {SDL_SCANCODE_F3, 0x3d, 0x72}, {SDL_SCANCODE_F4, 0x3e, 0x73}, {SDL_SCANCODE_F5, 0x3f, 0x74},
    {SDL_SCANCODE_F6, 0x40, 0x75}, {SDL_SCANCODE_F7, 0x41, 0x76}, {SDL_SCANCODE_F8, 0x42, 0x77},
    {SDL_SCANCODE_F9, 0x43, 0x78}, {SDL_SCANCODE_F10, 0x44, 0x79}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x45, 0x90},
    {SDL_SCANCODE_SCROLLLOCK, 0x46, 0x91}, {SDL_SCANCODE_KP_7, 0x47, 0x67}, {SDL_SCANCODE_KP_8, 0x48, 0x68},
    {SDL_SCANCODE_KP_9, 0x49, 0x69}, {SDL_SCANCODE_KP_MINUS, 0x4a, 0x6d}, {SDL_SCANCODE_KP_4, 0x4b, 0x64},
    {SDL_SCANCODE_KP_5, 0x4c, 0x65}, {SDL_SCANCODE_KP_6, 0x4d, 0x66}, {SDL_SCANCODE_KP_PLUS, 0x4e, 0x6b},
    {SDL_SCANCODE_KP_1, 0x4f, 0x61}, {SDL_SCANCODE_KP_2, 0x50, 0x62}, {SDL_SCANCODE_KP_3, 0x51, 0x63},
    {SDL_SCANCODE_KP_0, 0x52, 0x60}, {SDL_SCANCODE_KP_PERIOD, 0x53, 0x6e}, {SDL_SCANCODE_NONUSBACKSLASH, 0x56, 0xe2},
    {SDL_SCANCODE_F11, 0x57, 0x7a}, {SDL_SCANCODE_F12, 0x58, 0x7b}, {SDL_SCANCODE_KP_ENTER, 0x9c, 0x0d},
    {SDL_SCANCODE_RCTRL, 0x9d, 0x11}, {SDL_SCANCODE_KP_DIVIDE, 0xb5, 0x6f}, {SDL_SCANCODE_PRINTSCREEN, 0xb7, 0x2c},
    {SDL_SCANCODE_RALT, 0xb8, 0x12}, {SDL_SCANCODE_PAUSE, 0xc5, 0x13}, {SDL_SCANCODE_HOME, 0xc7, 0x24},
    {SDL_SCANCODE_UP, 0xc8, 0x26}, {SDL_SCANCODE_PAGEUP, 0xc9, 0x21}, {SDL_SCANCODE_LEFT, 0xcb, 0x25},
    {SDL_SCANCODE_RIGHT, 0xcd, 0x27}, {SDL_SCANCODE_END, 0xcf, 0x23}, {SDL_SCANCODE_DOWN, 0xd0, 0x28},
    {SDL_SCANCODE_PAGEDOWN, 0xd1, 0x22}, {SDL_SCANCODE_INSERT, 0xd2, 0x2d}, {SDL_SCANCODE_DELETE, 0xd3, 0x2e},
    {SDL_SCANCODE_LGUI, 0xdb, 0x5b}, {SDL_SCANCODE_RGUI, 0xdc, 0x5c}, {SDL_SCANCODE_APPLICATION, 0xdd, 0x5d},
};

const Key* key_for(SDL_Scancode sc) {
    for (const Key& k : KEYS)
        if (k.sc == sc) return &k;
    return 0;
}

// ---- the window --------------------------------------------------------------------------------------
void clip_cursor(bool on) {                                      // restrict_cursor: the game's picture
    if (!g_window) return;
    if (vp_two_copies()) on = false;                             // [test] two_copies: the mouse goes between the windows
    SDL_Rect r = {0, 0, 640, 480};                               // DirectDraw: the top-left 640x480
    if (g_view.set) r = {g_view.x0, g_view.y0, g_view.w, g_view.h};
    SDL_SetWindowMouseRect(g_window, on ? &r : 0);
}

// [test] two_copies: the game's own cursor clip, left out. app_begin ends with Win32RestrictCursor, which clips the
// cursor to the SCREEN's top-left 640x480 (restrict_cursor, through the game's ClipCursor import -- the rewrite's call
// reads the same slot). Full screen, SDL replaces that clip with the window's own (SDL_SetWindowMouseRect, re-applied
// every few seconds). In two_copies there is no window clip, and SDL only ever frees a clip it made itself, so the
// game's stayed until Windows dropped it at the next switch between programs: the mouse reached only the part of a
// window inside the screen's top-left 640x480 (copy 1: its top-left corner; copy 2, on the right half: none of it)
// until Alt-Tab. A clip request is ignored; freeing goes through.
BOOL WINAPI copies_clip_cursor(const RECT* r) {
    if (!r) return vpos_ClipCursor(0);
    static bool said;
    if (!said) {
        said = true;
        logf("two_copies: the game's cursor clip (%ld,%ld)-(%ld,%ld) left out -- the mouse goes between the windows",
             r->left, r->top, r->right, r->bottom);
    }
    return TRUE;
}

// Switching away and back: the game pauses on exactly WM_ACTIVATEAPP, as win32_event did. SDL's own
// focus events don't line up with it while DirectDraw holds the screen, and drawing on after the
// switch meets lost surfaces (DDERR_WRONGMODE) and crashes. WM_ACTIVATEAPP is sent, not posted, so it
// never passes SDL's message hook (which sees only what its loop takes from the queue): the window
// procedure is subclassed for it. (Windows only; R2b: SDL's SDL_WINDOWEVENT_FOCUS_GAINED / FOCUS_LOST, see below.)
#ifdef _WIN32
static WNDPROC g_sdl_wndproc;
#endif

// what switching away or back does to the game: its inactive flag, and the switch-away pause
void set_game_active(bool active) {
    // (the physics clock: the original's physics runs on its own timer, so a race carries on while away)
    int tick = *(volatile int*)0x0052161c;
    if (!active) logf("platform: switched away (physics tick %d)", tick);
    else if (*(uint8_t*)G.inactive) logf("platform: switched back (physics tick %d)", tick);
    *(uint8_t*)G.inactive = active ? 0 : 1;
    // FIX: the original's physics runs on its own timer, so a race carried on unseen while the game was away.
    // A single-player race now pauses as the Esc menu pauses it (never a network race, which can't wait); the
    // unpause resynchronises the physics clock, so nothing is caught up on the way back.
    if (PhysicsPause && PhysicsUnpause && PhysicsIsPaused && MultiEnabled && g_phys_state) {
        if (!active && !g_away_paused && *(volatile int32_t*)g_phys_state == 3 && !MultiEnabled() &&
            !PhysicsIsPaused()) {
            PhysicsPause();
            g_away_paused = true;
            logf("platform: the race is paused while the game is away");
        } else if (active && g_away_paused) {
            PhysicsUnpause();
            g_away_paused = false;
        }
    }
}

#ifdef _WIN32
static LRESULT CALLBACK game_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ACTIVATEAPP) {
        bool active = wparam != 0;
        if (active) g_n_activations++;
        if (!session_playing()) {                                // a session replay: the recording's switches, not these
            session_active(active);
            set_game_active(active);
        }
        clip_cursor(active);
        if (active && g_gl && g_window) SDL_RaiseWindow(g_window);
    }
    return CallWindowProcA(g_sdl_wndproc, hwnd, msg, wparam, lparam);
}
#endif

void SDLCALL raw_message(void*, void*, unsigned int msg, Uint64 wparam, Sint64 lparam) {
    MsgHook_t* hooks = (MsgHook_t*)G.hooks;                      // Win32RegisterMessageHook's two slots
    for (int i = 0; i < 2; i++)
        if (hooks[i]) hooks[i](msg, (int)wparam, (int)lparam);
}

#ifndef _WIN32
// R2b (Linux): what Windows' window manager did for the game's window, from SDL's events. Activation: game_wndproc's
// WM_ACTIVATEAPP from the game window's focus (Windows sends it only on a change: so here); a message posted to the
// game's window (w32_set_message_poster, from any thread): an SDL event of its own, handed to the game's message hooks
// where SDL's Windows message hook would have. Both also while a session replay feeds the input (as on Windows).
Uint32 g_posted_event;

void activate(bool active) {
    static int last = -1;
    if (last == (int)active) return;
    last = active;
    if (active) g_n_activations++;
    if (!session_playing()) {                                    // a session replay: the recording's switches, not these
        session_active(active);
        set_game_active(active);
    }
    clip_cursor(active);
    if (active && g_gl && g_window) SDL_RaiseWindow(g_window);
}

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, int32_t lparam) {
    SDL_Event e;
    SDL_zero(e);
    e.type = g_posted_event;
    e.user.code = (Sint32)msg;
    e.user.data1 = (void*)(uintptr_t)wparam;
    e.user.data2 = (void*)(intptr_t)lparam;
    (void)hwnd;                                                  // (the game's window: the only one with a queue)
    SDL_PushEvent(&e);
}

// true if it was one of these
bool system_event(const SDL_Event& e) {
    if (e.type == SDL_WINDOWEVENT) {
        if (!g_window || e.window.windowID != SDL_GetWindowID(g_window)) return true;   // (the loading window's)
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) activate(true);
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) activate(false);
        return true;
    }
    if (g_posted_event && e.type == g_posted_event) {
        raw_message(0, 0, (unsigned)e.user.code, (Uint64)(uintptr_t)e.user.data1, (Sint64)(intptr_t)e.user.data2);
        return true;
    }
    return false;
}
#endif

// [test] two_copies (viperport.ini; README, "Testing multiplayer on one PC"): both copies are windows side by side on
// the primary display's usable area (without the taskbar), copy 1 on the left half and copy 2 on the right, each
// the largest 4:3 or 16:9 picture that fits its half with the window's frame, centred in it. The OpenGL renderer
// follows the window's size (gl_core.cpp make_target, Hor+ for 16:9) and hands the picture's place back through
// platform_set_view, which the mouse is mapped by. *w, *h: the picture's size.
void place_copy_window(SDL_Window* win, int* w, int* h) {
    SDL_Rect area = {0, 0, vpos_GetSystemMetrics(SM_CXSCREEN), vpos_GetSystemMetrics(SM_CYSCREEN)};
    SDL_GetDisplayUsableBounds(0, &area);
    int top = 0, left = 0, bottom = 0, right = 0;
#ifdef _WIN32
    if (SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right) != 0) {
        RECT r = {0, 0, 640, 480};                               // (SDL can't tell yet: ask Windows for the frame)
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE, 0);
        top = -r.top, left = -r.left, bottom = r.bottom - 480, right = r.right - 640;
    }
#else
    SDL_GetWindowBordersSize(win, &top, &left, &bottom, &right);  // (R2b: 0s until the window manager has framed it)
#endif
    const int half = area.w / 2;
    const int aw = half - left - right, ah = area.h - top - bottom;   // the room for the picture
    int best_w = 0, best_h = 0;
    static const int aspect[2][2] = {{4, 3}, {16, 9}};
    for (const auto& a : aspect) {
        int cw = aw, ch = aw * a[1] / a[0];
        if (ch > ah) ch = ah, cw = ah * a[0] / a[1];
        if (cw > 0 && ch > 0 && cw * ch > best_w * best_h) best_w = cw, best_h = ch;
    }
    if (best_w < 320 || best_h < 240) best_w = 640, best_h = 480;   // (a tiny screen: the game's own size)
    const int x0 = area.x + (vp_copy() == 2 ? half : 0);
    const int x = x0 + (half - (best_w + left + right)) / 2 + left;
    const int y = area.y + (area.h - (best_h + top + bottom)) / 2 + top;
    SDL_SetWindowSize(win, best_w, best_h);
    SDL_SetWindowPosition(win, x, y);
    SDL_ShowWindow(win);
    *w = best_w, *h = best_h;
    // the mouse's map until the renderer's first frame hands over its own (platform_set_view): the game's 640x480 as
    // make_target fits it into the window (gl_core.cpp) -- without it the moves before that frame reached the game in
    // window pixels (a 1106,992 on a 640x480 screen)
    float scale = best_h / 480.0f;
    if (640 * scale > best_w) scale = best_w / 640.0f;
    const float ox = (best_w - 640 * scale) * 0.5f;
    const int vx0 = (int)floorf(ox + 0.5f), vx1 = (int)floorf(ox + 640 * scale + 0.5f);
    g_view = {vx0, 0, vx1 - vx0, (int)floorf(480 * scale + 0.5f), 640, 480, true};
    logf("two_copies: copy %d's window %dx%d at (%d, %d), on the %s half of %dx%d at (%d, %d)", vp_copy(), best_w,
         best_h, x, y, vp_copy() == 2 ? "right" : "left", area.w, area.h, area.x, area.y);
}

unsigned char __cdecl sdl_create_window(void* instance) {
    *(HWND*)G.prev_foreground = vpos_GetForegroundWindow();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // DirectInput had the joystick in background mode
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    // the game's own window class name, so start_unique_instance's FindWindow still finds a running copy
    const char* cls = *(const char**)G.class_name;
#ifdef _WIN32
    SDL_RegisterApp(cls ? cls : "Viper Racing Window", CS_HREDRAW | CS_VREDRAW, instance);
#else
    (void)cls;
#endif
    // keep the game DPI-unaware, as it always was: DirectDraw's fullscreen and the mouse coordinates are
    // built on it (stage 2, with our own renderer, is where the game learns real pixels)
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "unaware");
    int before_w = vpos_GetSystemMetrics(SM_CXSCREEN), before_h = vpos_GetSystemMetrics(SM_CYSCREEN);
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        // (Linux without the 32-bit libudev: SDL's joystick driver can't start, and that stopped SDL as a whole; the
        // game then runs with the mouse and keyboard only)
        logf("SDL: controllers can't start (%s) -- the mouse and keyboard only", SDL_GetError());
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            logf("SDL: can't start: %s", SDL_GetError());
            return 0;
        }
    }
#ifdef _WIN32
    SDL_SetWindowsMessageHook(raw_message, 0);
#endif
    const char* title = *(const char**)G.title;
    int w = vpos_GetSystemMetrics(SM_CXSCREEN), h = vpos_GetSystemMetrics(SM_CYSCREEN);
    if (vp_two_copies()) {                                       // [test] two_copies: a window on its half
        // (the first copy keeps the game's title, which start_unique_instance's FindWindow looks for)
        g_window = SDL_CreateWindow(vp_copy() == 2 ? "Viper Racing (copy 2)" : title ? title : "Viper Racing", 0, 0,
                                    640, 480, SDL_WINDOW_HIDDEN | (g_gl ? SDL_WINDOW_OPENGL : 0));
        if (g_window) place_copy_window(g_window, &w, &h);
    } else {
        g_window = SDL_CreateWindow(title ? title : "Viper Racing", 0, 0, w, h,
                                    SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN |
                                    // DirectDraw minimised the game when it lost focus, so "on top" was harmless;
                                    // an OpenGL window stays put, and on top it would cover whatever you switch to
                                    (g_gl ? SDL_WINDOW_OPENGL : SDL_WINDOW_ALWAYS_ON_TOP));
    }
    if (!g_window) {
        logf("SDL: can't create the window: %s", SDL_GetError());
        return 0;
    }
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    SDL_GetWindowWMInfo(g_window, &info);
    g_hwnd = info.info.win.window;
    g_sdl_wndproc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)game_wndproc);
    HICON icon = vpos_LoadIconA((HINSTANCE)instance, MAKEINTRESOURCEA(1));
    if (icon) {
        SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
#else
    // R2b: the game's HWND -- a fake one the window stand-ins (w32_user.h) know as this SDL window, by the game's class
    // and the window's title (start_unique_instance's FindWindowA); switching away and back from SDL's focus events
    // (system_event: what game_wndproc does on WM_ACTIVATEAPP); the icon from race.exe's icon group 1; the replies
    // winsock's stand-in posts to the game's window (an async host lookup) reach the game's message hooks through
    // SDL's queue, in order with everything else (system_event), as SDL's Windows message hook hands them on
    g_hwnd = (HWND)(uintptr_t)w32_adopt_window(g_window, cls ? cls : "Viper Racing Window", SDL_GetWindowTitle(g_window));
    if (SDL_Surface* icon = w32_resource_icon((uint32_t)(uintptr_t)instance, 1)) {
        SDL_SetWindowIcon(g_window, icon);
        SDL_FreeSurface(icon);
    } else {
        logf("SDL: race.exe's icon wasn't found");
    }
    if (!g_posted_event) g_posted_event = SDL_RegisterEvents(1);
    if (g_posted_event != (Uint32)-1) w32_set_message_poster(post_message);
#endif
    *(HWND*)G.hwnd = g_hwnd;
    SDL_ShowCursor(SDL_DISABLE);                                 // the game draws its own
    SDL_RaiseWindow(g_window);
    logf("SDL: window %dx%d (HWND %p; the screen measured %dx%d before SDL started), SDL %d.%d.%d", w, h, g_hwnd,
         before_w, before_h, SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    return 1;
}

void __cdecl sdl_hook_keys(void) {}                              // the game's hook passed everything on anyway

void __cdecl sdl_mouse_center(void) {                            // MouseCenter: the middle of the picture
    if (!g_window || session_playing()) return;                  // (a replay leaves the player's mouse alone)
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    if (g_view.set) SDL_WarpMouseInWindow(g_window, g_view.x0 + g_view.w / 2, g_view.y0 + g_view.h / 2);
    else SDL_WarpMouseInWindow(g_window, w / 2, h / 2);
}

// ---- events ------------------------------------------------------------------------------------------
// the game's input calls, each recorded in a session (an op of the Win32Idle call it's made in)
void key_down(unsigned vk) { uint8_t b = (uint8_t)vk; session_op(SOP_KEY_DOWN, &b, 1); KeyDown(vk); }
void key_up(unsigned vk) { uint8_t b = (uint8_t)vk; session_op(SOP_KEY_UP, &b, 1); KeyUp(vk); }
void key_char(unsigned ch, unsigned char scan) { uint8_t b[2] = {(uint8_t)ch, scan}; session_op(SOP_KEY_CHAR, b, 2); KeyQueueChar(ch, scan); }
void key_meta(unsigned vk, unsigned char scan) { uint8_t b[2] = {(uint8_t)vk, scan}; session_op(SOP_KEY_META, b, 2); KeyQueueMetaChar(vk, scan); }
#pragma pack(push, 1)
struct MouseOp { uint8_t type; int16_t x, y; uint8_t buttons; };
#pragma pack(pop)
void mouse_event(int type, int x, int y, int buttons) {
    MouseOp m = {(uint8_t)type, (int16_t)x, (int16_t)y, (uint8_t)buttons};
    session_op(SOP_MOUSE, &m, sizeof m);
    MouseQueueEvent(type, x, y, buttons);
}

void queue_text(const char* utf8) {                              // typed text: the game's character set is ANSI
    WCHAR wide[32];                                              // (16-bit: wchar_t on Windows)
    int n = vpos_MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 32);
    char ansi[64];
    int m = n > 0 ? vpos_WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, sizeof ansi, 0, 0) : 0;
    for (int i = 0; i + 1 < m; i++) key_char((unsigned char)ansi[i], 0);
}

void mouse(int type, int x, int y) {
    if (g_view.set && g_view.w > 0 && g_view.h > 0) {            // window pixels -> the game's own
        x = (x - g_view.x0) * g_view.game_w / g_view.w;
        y = (y - g_view.y0) * g_view.game_h / g_view.h;
        x = x < 0 ? 0 : x >= g_view.game_w ? g_view.game_w - 1 : x;
        y = y < 0 ? 0 : y >= g_view.game_h ? g_view.game_h - 1 : y;
    }
    mouse_event(type, x, y, (int)g_mouse_buttons);
}

void handle(const SDL_Event& e) {
#ifndef _WIN32
    if (system_event(e)) return;                                 // (R2b: activation, posted messages)
#endif
    switch (e.type) {
    case SDL_QUIT:                                               // WM_DESTROY, then WM_QUIT -> ExitProcess
        session_op(SOP_QUIT);
        clip_cursor(false);
        SDL_ShowCursor(SDL_ENABLE);
        vpos_ExitProcess(0);
    case SDL_KEYDOWN: {                                          // WM_KEYDOWN / WM_SYSKEYDOWN
        g_n_keys++;
        const Key* k = key_for(e.key.keysym.scancode);
        if (!k) break;
        unsigned char scan = k->dik & 0x7f;
        key_down(k->vk);
        key_meta(k->vk, scan);
        // the characters WM_CHAR delivered that SDL's text input doesn't: control keys and ctrl+letters
        if (k->vk == 0x08 || k->vk == 0x09 || k->vk == 0x0d || k->vk == 0x1b)
            key_char(k->vk, scan);
        else if ((e.key.keysym.mod & KMOD_CTRL) && !(e.key.keysym.mod & KMOD_ALT) && k->vk >= 'A' && k->vk <= 'Z')
            key_char(k->vk - 0x40, scan);
        break;
    }
    case SDL_KEYUP: {
        const Key* k = key_for(e.key.keysym.scancode);
        if (k) key_up(k->vk);
        break;
    }
    case SDL_TEXTINPUT:
        queue_text(e.text.text);
        break;
    case SDL_MOUSEMOTION:
        g_n_motion++;
        mouse(6, e.motion.x, e.motion.y);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        bool down = e.type == SDL_MOUSEBUTTONDOWN;
        if (down) g_n_clicks++;
        int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2
                : e.button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
        if (!bit) break;
        g_mouse_buttons = down ? (g_mouse_buttons | bit) : (g_mouse_buttons & ~bit);
        int type = (bit == 1 ? 0 : bit == 2 ? 2 : 4) + (down ? 0 : 1);   // L down/up 0/1, R 2/3, M 4/5
        mouse(type, e.button.x, e.button.y);
        break;
    }
    }
}

void scan_update() {                                             // ScanUpdate: the DirectInput keyboard state
    uint8_t* state = (uint8_t*)G.dik;
    memset(state, 0, 256);
    const Uint8* ks = SDL_GetKeyboardState(0);
    for (const Key& k : KEYS)
        if (ks[k.sc]) state[k.dik] = 0x80;
    *(uint8_t*)G.shift = state[0x2a] | state[0x36];
    *(uint8_t*)G.ctrl = state[0x1d] | state[0x9d];
    *(uint8_t*)G.alt = state[0x38] | state[0xb8];
}

// a session: the keys held after a Win32Idle, as the DirectInput codes down
void record_scan() {
    if (!session_recording()) return;
    const uint8_t* state = (const uint8_t*)G.dik;
    uint8_t list[1 + 255];
    int n = 0;
    for (int i = 0; i < 256 && n < 255; i++)
        if (state[i]) list[1 + n++] = (uint8_t)i;
    list[0] = (uint8_t)n;
    session_op(SOP_SCAN, list, (uint8_t)(n + 1 > 255 ? 255 : n + 1));
}

void __cdecl sdl_idle(void) {                                    // Win32Idle
    SDL_Event e;
    if (session_playing()) {                                     // a session replay: the recording's input
        while (SDL_PollEvent(&e))
#ifndef _WIN32
            if (e.type == SDL_QUIT) handle(e);                   // (closing the window still quits)
            else system_event(e);                                // (R2b: activation, posted messages, as on Windows)
#else
            if (e.type == SDL_QUIT) handle(e);                   // (closing the window still quits)
#endif
        if (session_play_idle()) return;
    }                                                            // (the recording ran out: the player's, from here)
    session_idle_begin();
    if (vp_two_copies()) {
        // [test] two_copies: a copy switched away doesn't wait -- the other copy is the one in front, and a network
        // game has to keep running in both. What the wait did on the way back happens when the copy is switched back.
        static bool away;
        if (*(uint8_t*)G.inactive) away = true;
        else if (away) {
            away = false;
            session_op(SOP_CLEAR_BITS);
            KeyClearBits();
            session_op(SOP_RESTORE);
            gxRestore();
        }
    } else if (*(uint8_t*)G.inactive) {                          // switched away: wait to be switched back
        DWORD t0 = vpos_GetTickCount();
        if (g_gl) gfx_repaint();                                 // the taskbar's preview: the race, not a menu
        while (*(uint8_t*)G.inactive && SDL_WaitEvent(&e)) handle(e);
        logf("platform: the game loop waited %lu ms while switched away", vpos_GetTickCount() - t0);
        session_op(SOP_CLEAR_BITS);
        KeyClearBits();
        session_op(SOP_RESTORE);
        gxRestore();
    }
    while (SDL_PollEvent(&e)) handle(e);
    scan_update();
    record_scan();
    session_idle_end();
}

void __cdecl sdl_scan_begin(void) { memset((void*)G.dik, 0, 256); }
void __cdecl sdl_scan_update(void) {                             // (only the original Win32Idle calls it)
    if (session_playing() && session_play_idle()) return;
    session_idle_begin();
    scan_update();
    record_scan();
    session_idle_end();
}

// ---- joysticks and wheels ----------------------------------------------------------------------------
// JoyPos, as JoyGetPos fills it from DirectInput's DIJOYSTATE: axes X Y Z Rz Rx Ry in -1..1, 32 buttons,
// then the POV hat as 0 (centred) or 1..4 (up, right, down, left).
struct JoyPos { float axis[6]; uint8_t button[32]; int32_t pov; };   // pov: an int, as the original stores it

float unit(Sint16 v) { float f = v / 32768.0f; return f < -1 ? -1 : f > 1 ? 1 : f; }

void __cdecl sdl_joy_begin(void) {
    g_joy_name[0] = 0;
    for (int i = 0; i < SDL_NumJoysticks() && !g_joy; i++) {
        if (SDL_IsGameController(i) && (g_pad = SDL_GameControllerOpen(i)))
            g_joy = SDL_GameControllerGetJoystick(g_pad);
        else
            g_joy = SDL_JoystickOpen(i);
    }
    if (!g_joy) {
        logf("SDL: no joystick");
        return;
    }
    const char* n = g_pad ? SDL_GameControllerName(g_pad) : SDL_JoystickName(g_joy);
    strncpy(g_joy_name, n ? n : "Joystick", sizeof g_joy_name - 1);
    if (SDL_JoystickIsHaptic(g_joy) == 1 && (g_haptic = SDL_HapticOpenFromJoystick(g_joy))) {
        if (SDL_HapticQuery(g_haptic) & SDL_HAPTIC_CONSTANT) {
            SDL_HapticEffect fx;
            memset(&fx, 0, sizeof fx);
            fx.type = SDL_HAPTIC_CONSTANT;
            fx.constant.direction.type = SDL_HAPTIC_CARTESIAN;
            fx.constant.direction.dir[0] = 1;
            fx.constant.length = SDL_HAPTIC_INFINITY;
            g_effect = SDL_HapticNewEffect(g_haptic, &fx);
        }
    }
    logf("SDL: joystick '%s' (%s, %d axes, %d buttons%s)", g_joy_name, g_pad ? "game controller" : "joystick",
         SDL_JoystickNumAxes(g_joy), SDL_JoystickNumButtons(g_joy), g_effect >= 0 ? ", force feedback" : "");
}

void __cdecl sdl_joy_end(void) {
    if (g_haptic) SDL_HapticClose(g_haptic);
    if (g_pad) SDL_GameControllerClose(g_pad);
    else if (g_joy) SDL_JoystickClose(g_joy);
    g_haptic = 0, g_pad = 0, g_joy = 0, g_effect = -1;
}

unsigned char joy_get_pos(JoyPos* p);
struct JoyRec { uint8_t ok; JoyPos pos; };
unsigned char __cdecl sdl_joy_get_pos(JoyPos* p) {               // in a session: recorded, and fed back
    JoyRec r;
    if (session_feed(SK_JOYPOS, &r, sizeof r)) {
        if (r.ok) *p = r.pos;
        return r.ok;
    }
    memset(&r, 0, sizeof r);
    r.ok = joy_get_pos(&r.pos);
    if (r.ok) *p = r.pos;
    session_saw(SK_JOYPOS, &r, sizeof r);
    return r.ok;
}

unsigned char joy_get_pos(JoyPos* p) {
    if (!g_joy) return 0;
    memset(p, 0, sizeof *p);
    if (g_pad) {
        // the layout Windows' own DirectInput driver gives an XInput pad, so existing control setups still fit:
        // X/Y left stick, Z both triggers (left one positive), Rx/Ry right stick, buttons A B X Y LB RB Back Start LS RS
        SDL_GameController* c = g_pad;
        p->axis[0] = unit(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX));
        p->axis[1] = unit(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
        float lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
        float rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
        p->axis[2] = lt - rt;
        p->axis[4] = unit(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX));
        p->axis[5] = unit(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
        static const SDL_GameControllerButton order[] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
            SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK};
        for (int i = 0; i < 10; i++) p->button[i] = SDL_GameControllerGetButton(c, order[i]) ? 1 : 0;
        bool up = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
        bool dn = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
        bool lf = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
        bool rt_ = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
        int deg = up ? (rt_ ? 45 : lf ? 315 : 0) : dn ? (rt_ ? 135 : lf ? 225 : 180) : rt_ ? 90 : lf ? 270 : -1;
        p->pov = deg < 0 ? 0 : deg * 100 / 9000 + 1;
    } else {
        // a plain joystick or wheel: SDL's axes come in DirectInput's order X Y Z Rx Ry Rz
        static const int from[6] = {0, 1, 2, 5, 3, 4};           // JoyPos wants X Y Z Rz Rx Ry
        int n = SDL_JoystickNumAxes(g_joy);
        for (int i = 0; i < 6; i++) p->axis[i] = from[i] < n ? unit(SDL_JoystickGetAxis(g_joy, from[i])) : 0.0f;
        int nb = SDL_JoystickNumButtons(g_joy);
        for (int i = 0; i < 32 && i < nb; i++) p->button[i] = SDL_JoystickGetButton(g_joy, i) ? 1 : 0;
        if (SDL_JoystickNumHats(g_joy) > 0) {
            Uint8 h = SDL_JoystickGetHat(g_joy, 0);
            int deg = h == SDL_HAT_UP ? 0 : h == SDL_HAT_RIGHTUP ? 45 : h == SDL_HAT_RIGHT ? 90 : h == SDL_HAT_RIGHTDOWN ? 135
                    : h == SDL_HAT_DOWN ? 180 : h == SDL_HAT_LEFTDOWN ? 225 : h == SDL_HAT_LEFT ? 270 : h == SDL_HAT_LEFTUP ? 315 : -1;
            p->pov = deg < 0 ? 0 : deg * 100 / 9000 + 1;
        }
    }
    return 1;
}

char g_fed_joy_name[128];
const char* __cdecl sdl_joy_get_name(void) {
    if (session_feed(SK_JOYNAME, g_fed_joy_name, sizeof g_fed_joy_name)) return g_fed_joy_name;
    session_saw(SK_JOYNAME, g_joy_name, sizeof g_joy_name);
    return g_joy_name;
}
unsigned char __cdecl sdl_joy_has_ff(void) {
    uint8_t v;
    if (session_feed(SK_JOYFF, &v, 1)) return v;
    v = g_effect >= 0;
    session_saw(SK_JOYFF, &v, 1);
    return v;
}

void __cdecl sdl_joy_enable_ff(unsigned char on) {
    if ((on != 0) == g_ff_on) return;
    g_ff_on = on != 0;
    if (g_effect < 0) return;
    if (SDL_HapticQuery(g_haptic) & SDL_HAPTIC_AUTOCENTER) SDL_HapticSetAutocenter(g_haptic, g_ff_on ? 0 : 100);
    if (g_ff_on) SDL_HapticRunEffect(g_haptic, g_effect, 1);
    else SDL_HapticStopEffect(g_haptic, g_effect);
}

void __cdecl sdl_joy_set_force(float a, float b, float c) {
    // JoySetForce with one (x) effect: magnitude = (a + (b - last b) + (c - last c)) x 10000, clamped to
    // DirectInput's +-10000 -- scaled here to SDL's +-32767
    if (!g_ff_on || g_effect < 0) return;
    float x = a + (b - g_ff_prev_b) + (c - g_ff_prev_c);
    g_ff_prev_b = b, g_ff_prev_c = c;
    float m = x * 10000.0f;
    m = m > 10000 ? 10000 : m < -10000 ? -10000 : m;
    SDL_HapticEffect fx;
    memset(&fx, 0, sizeof fx);
    fx.type = SDL_HAPTIC_CONSTANT;
    fx.constant.direction.type = SDL_HAPTIC_CARTESIAN;
    fx.constant.direction.dir[0] = 1;
    fx.constant.length = SDL_HAPTIC_INFINITY;
    fx.constant.level = (Sint16)(m * 32767.0f / 10000.0f);
    SDL_HapticUpdateEffect(g_haptic, g_effect, &fx);
}

}  // namespace

// a session replay: one recorded op of a Win32Idle call, handed to the game as the live code hands it (session.h)
void platform_session_apply(uint8_t op, const uint8_t* p, uint8_t n) {
    switch (op) {
    case SOP_KEY_DOWN: if (n >= 1) KeyDown(p[0]); break;
    case SOP_KEY_UP: if (n >= 1) KeyUp(p[0]); break;
    case SOP_KEY_CHAR: if (n >= 2) KeyQueueChar(p[0], p[1]); break;
    case SOP_KEY_META: if (n >= 2) KeyQueueMetaChar(p[0], p[1]); break;
    case SOP_MOUSE:
        if (n >= sizeof(MouseOp)) {
            MouseOp m;
            memcpy(&m, p, sizeof m);
            MouseQueueEvent(m.type, m.x, m.y, m.buttons);
        }
        break;
    case SOP_ACTIVE: if (n >= 1) set_game_active(p[0] != 0); break;
    case SOP_CLEAR_BITS: KeyClearBits(); break;
    case SOP_RESTORE: gxRestore(); break;
    case SOP_SCAN: {
        uint8_t* state = (uint8_t*)G.dik;
        memset(state, 0, 256);
        for (int i = 0; n >= 1 && i < p[0] && 1 + i < n; i++) state[p[1 + i]] = 0x80;
        *(uint8_t*)G.shift = state[0x2a] | state[0x36];
        *(uint8_t*)G.ctrl = state[0x1d] | state[0x9d];
        *(uint8_t*)G.alt = state[0x38] | state[0xb8];
        break;
    }
    case SOP_QUIT:
        logf("platform: the recording closed the game here");
        clip_cursor(false);
        SDL_ShowCursor(SDL_ENABLE);
        vpos_ExitProcess(0);
    }
}

void platform_report() {
    if (g_window)
        logf("exit: input: %ld key presses, %ld mouse moves, %ld clicks, %ld activations", g_n_keys, g_n_motion,
             g_n_clicks, g_n_activations);
}

// ---- for the renderer (ddraw_gl.cpp) ------------------------------------------------------------------
SDL_Window* platform_window() { return g_window; }

void platform_set_view(int x0, int y0, int w, int h, int game_w, int game_h) {
    bool moved = !g_view.set || g_view.x0 != x0 || g_view.y0 != y0 || g_view.w != w || g_view.h != h;
    g_view = {x0, y0, w, h, game_w, game_h, true};
    if (moved && !*(uint8_t*)G.inactive) clip_cursor(true);
}

// ---- switching it on -------------------------------------------------------------------------------------
// viperport.exe (standalone.h): the game's own window, DirectDraw and DirectSound code is int3 there, so the platform is
// always SDL, the OpenGL renderer and SDL audio, whatever viperport.ini says
static int platform_ini_sdl(const char* ini) { return vp_standalone() ? 1 : vpos_GetPrivateProfileIntA("platform", "sdl", 0, ini); }
static void platform_ini_str(const char* key, const char* dflt, const char* standalone, char* out, DWORD n, const char* ini) {
    if (vp_standalone()) vpos_lstrcpynA(out, standalone, (int)n);
    else vpos_GetPrivateProfileStringA("platform", key, dflt, out, n, ini);
}

bool platform_switched_away() { return G.inactive && *(volatile uint8_t*)G.inactive; }

// SDL2.dll is there (the MSVC build delay-loads it; GCC links it, and a Linux build has it always)
#ifdef _WIN32
#define VP_SDL2_THERE() LoadLibraryA("SDL2.dll")
#else
#define VP_SDL2_THERE() true
#endif

bool platform_plans_gl(const char* ini) {
    char renderer[16];
    platform_ini_str("renderer", "ddraw", "gl", renderer, sizeof renderer, ini);
    return platform_ini_sdl(ini) && _stricmp(renderer, "gl") == 0 && VP_SDL2_THERE();
}

bool platform_plans_sdl_audio(const char* ini) {
    char audio[16];
    platform_ini_str("audio", "dsound", "sdl", audio, sizeof audio, ini);
    return platform_ini_sdl(ini) && _stricmp(audio, "sdl") == 0 && VP_SDL2_THERE();
}

void platform_install(const char* build) {
    char ini[MAX_PATH];
    HMODULE self = 0;                                            // this DLL (R2b: 0 -- the one executable)
#ifdef _WIN32
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&platform_install, &self);
#endif
    vpos_GetModuleFileNameA(self, ini, MAX_PATH);
    char* slash = strrchr(ini, '\\');
    vpos_lstrcpyA(slash ? slash + 1 : ini, "viperport.ini");
    if (!platform_ini_sdl(ini)) {
        logf("platform: the game's own (viperport.ini [platform] sdl=0)");
        return;
    }
    char renderer[16];
    platform_ini_str("renderer", "ddraw", "gl", renderer, sizeof renderer, ini);
    g_gl = _stricmp(renderer, "gl") == 0;
    if (!VP_SDL2_THERE()) {                             // delay-loaded: make sure it's there first
        logf("platform: NOT switching to SDL2 -- SDL2.dll isn't next to the game");
        return;
    }
    KeyDown = (U32_t)A(0x00413e20);
    KeyUp = (U32_t)A(0x00413e70);
    KeyQueueChar = (U32U8_t)A(0x00413dd0);
    KeyQueueMetaChar = (U32U8_t)A(0x00413df0);
    KeyClearBits = (Void_t)A(0x00413ec0);
    gxRestore = (Void_t)A(0x0044df00);
    MouseQueueEvent = (MouseQueue_t)A(0x00414530);
    PhysicsPause = (Void_t)A(0x0042bcc0);                        // (optional: without them a switch doesn't pause)
    PhysicsUnpause = (Void_t)A(0x0042bcf0);
    PhysicsIsPaused = (U8_t)A(0x0042bd20);
    MultiEnabled = (U8_t)A(0x004a23c0);
    g_phys_state = A(0x004ecf88);
    G.hwnd = A(0x004e5eac);
    G.prev_foreground = A(0x004e5eb0);
    G.inactive = A(0x004e5ec0);
    G.hooks = A(0x00508060);
    G.title = A(0x004db440);
    G.class_name = A(0x004db43c);
    G.dik = A(0x00508088);
    G.shift = A(0x00508185);
    G.ctrl = A(0x00508186);
    G.alt = A(0x00508187);
    if (!KeyDown || !KeyUp || !KeyQueueChar || !KeyQueueMetaChar || !KeyClearBits || !gxRestore || !MouseQueueEvent ||
        !G.hwnd || !G.prev_foreground || !G.inactive || !G.hooks || !G.title || !G.class_name || !G.dik || !G.shift || !G.ctrl || !G.alt) {
        logf("platform: NOT switching to SDL2 -- %s is missing addresses", build);
        return;
    }
    // first bytes of each replaced function in v1.0 (other builds check their own, from their tables)
    static const uint8_t create_window_pro[] = {0x83, 0xEC, 0x28, 0x53, 0x56, 0x57, 0x33, 0xF6};
    static const uint8_t hook_keys_pro[] = {0xFF, 0x15, 0x74, 0x74, 0x5D, 0x00};
    static const uint8_t idle_pro[] = {0x83, 0xEC, 0x1C, 0x80, 0x3D, 0xC0, 0x5E, 0x4E};
    static const uint8_t scan_begin_pro[] = {0x57, 0x6A, 0x00, 0x68, 0x88, 0x81, 0x50, 0x00};
    static const uint8_t scan_update_pro[] = {0x83, 0x3D, 0x8C, 0x81, 0x50, 0x00, 0x00, 0x0F};
    static const uint8_t mouse_center_pro[] = {0x6A, 0x00, 0xA1, 0x54, 0x86, 0x50, 0x00, 0x6A};
    static const uint8_t joy_begin_pro[] = {0x53, 0x33, 0xDB, 0x53, 0x88, 0x1D, 0x10, 0x91};
    static const uint8_t joy_pos_pro[] = {0x83, 0xEC, 0x54, 0x83, 0x3D, 0x08, 0x91, 0x50};
    static const uint8_t joy_name_pro[] = {0x83, 0x3D, 0x08, 0x91, 0x50, 0x00, 0x00, 0xB8};
    static const uint8_t joy_hasff_pro[] = {0x83, 0x3D, 0x14, 0x91, 0x50, 0x00, 0x00, 0x75};
    static const uint8_t joy_enff_pro[] = {0x8A, 0x44, 0x24, 0x04, 0x3A, 0x05, 0x10, 0x91};
    static const uint8_t joy_force_pro[] = {0x80, 0x3D, 0x10, 0x91, 0x50, 0x00, 0x00, 0x56};
    static const uint8_t joy_end_pro[] = {0x83, 0x3D, 0x14, 0x91, 0x50, 0x00, 0x00, 0x74};
    struct H { uint32_t at; const uint8_t* pro; size_t n; void* to; const char* what; };
    const H window[] = {
        {0x00412690, create_window_pro, sizeof create_window_pro, (void*)sdl_create_window, "create_window (SDL window)"},
        {0x00412ac0, hook_keys_pro, sizeof hook_keys_pro, (void*)sdl_hook_keys, "hook_keys"},
        {0x00412bf0, idle_pro, sizeof idle_pro, (void*)sdl_idle, "Win32Idle (SDL events)"},
        {0x00412f00, scan_begin_pro, sizeof scan_begin_pro, (void*)sdl_scan_begin, "ScanBegin"},
        {0x00413040, scan_update_pro, sizeof scan_update_pro, (void*)sdl_scan_update, "ScanUpdate (SDL keyboard)"},
        {0x00414620, mouse_center_pro, sizeof mouse_center_pro, (void*)sdl_mouse_center, "MouseCenter"},
    };
    // v1.0 and v1.1 have one joystick; 1.2.x rewrote joy.obj for up to 8 (every call takes an index), so
    // its tables have none of these and its joysticks stay on DirectInput
    const H joystick[] = {
        {0x00418bb0, joy_begin_pro, sizeof joy_begin_pro, (void*)sdl_joy_begin, "JoyBegin (SDL joystick)"},
        {0x00418f30, joy_end_pro, sizeof joy_end_pro, (void*)sdl_joy_end, "JoyEnd"},
        {0x00418ff0, joy_pos_pro, sizeof joy_pos_pro, (void*)sdl_joy_get_pos, "JoyGetPos"},
        {0x00418fd0, joy_name_pro, sizeof joy_name_pro, (void*)sdl_joy_get_name, "JoyGetName"},
        {0x00419230, joy_hasff_pro, sizeof joy_hasff_pro, (void*)sdl_joy_has_ff, "JoyHasForceFeedback"},
        {0x00419250, joy_enff_pro, sizeof joy_enff_pro, (void*)sdl_joy_enable_ff, "JoyEnableForceFeedback"},
        {0x00419360, joy_force_pro, sizeof joy_force_pro, (void*)sdl_joy_set_force, "JoySetForce"},
    };
    for (const H& h : window)                                    // all or nothing: check every one first
        if (!code_is(h.at, h.pro, h.n)) {
            logf("platform: NOT switching to SDL2 -- %s isn't the code %s should have", h.what, build);
            return;
        }
    bool joy = true;
    for (const H& h : joystick)
        joy = joy && have(h.at) && code_is(h.at, h.pro, h.n);
    for (const H& h : window) jmp_hook(h.at, h.pro, h.n, h.to, h.what);
    if (joy)
        for (const H& h : joystick) jmp_hook(h.at, h.pro, h.n, h.to, h.what);
    else
        logf("platform: joysticks stay on DirectInput in %s (its joystick code isn't v1.0's)", build);
    logf("platform: SDL2 window, keyboard and mouse%s", joy ? ", joystick" : "");
    if (g_gl && !renderer_install()) g_gl = false;               // M2 stage 2: OpenGL in place of DirectDraw
    if (vp_two_copies() && !g_gl)
        logf("two_copies: the windows need [platform] renderer=gl -- DirectDraw takes the whole screen for itself");
    if (vp_two_copies() && !patch_import("USER32.dll", "ClipCursor", (void*)copies_clip_cursor))
        logf("two_copies: the game's ClipCursor import wasn't found -- its cursor clip stays until a switch away");
    if (!g_gl && platform_plans_gl(ini))
        logf("platform: the OpenGL renderer isn't on after all, but the dd.obj rewrites expect it: set [port] "
             "default=original for dd.obj's functions, or fix what the log says above");
    char audio[16];
    platform_ini_str("audio", "dsound", "sdl", audio, sizeof audio, ini);
    const bool sdl_audio = _stricmp(audio, "sdl") == 0 && audio_install();   // M2 stage 3: SDL audio in place of DirectSound
    if (!sdl_audio && platform_plans_sdl_audio(ini))
        logf("platform: the SDL audio isn't on after all, but the wave.obj rewrites expect it: set [port] "
             "default=original for wave.obj's functions, or fix what the log says above");
}
