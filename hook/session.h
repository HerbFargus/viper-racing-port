// session.h -- M3 UI step R: the session recorder (session.cpp). A whole run of the game, menus included, recorded
// and replayed: what the main thread reads from outside, in the order it reads it, and a per-frame hash of what the
// game hands the renderer. See session.cpp for the design.
//
// Everything here is a no-op, and nothing is hooked, unless viperport.ini has `[session] record=1` or `play=<name>`.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SESSION_OFF, SESSION_RECORD, SESSION_PLAY, SESSION_ENDED };   // ENDED: a replay whose recording ran out
extern volatile int g_session_mode;
extern bool g_session_frames;                  // hashing the frames (record or play, on the OpenGL renderer)

void session_install(const char* ini);         // after port_install: reads [session], hooks what it needs
void session_report();                         // the exit log (and a recording's end)

inline bool session_recording() { return g_session_mode == SESSION_RECORD; }
inline bool session_playing() { return g_session_mode == SESSION_PLAY; }

// ---- reads the platform layer hands the game (platform.cpp) -----------------------------------------------------------
// On the session's thread (the main thread), a read goes through the stream: session_feed fills v from the recording
// and returns true while a replay feeds; otherwise the caller reads the real thing and passes it to session_saw, which
// records it. Kinds: see session.cpp (SK_*).
enum : uint8_t { SK_JOYPOS = 'J', SK_JOYNAME = 'N', SK_JOYFF = 'H' };
bool session_feed(uint8_t kind, void* v, size_t n);
void session_saw(uint8_t kind, const void* v, size_t n);

// Win32Idle: every input event the platform layer hands the game in one call, as "ops" (SOP_*): recorded between
// session_idle_begin and session_idle_end; a replay's Win32Idle calls session_play_idle instead, which hands the
// recording's ops to platform_session_apply (false once the recording has run out: the player's input from then on).
enum : uint8_t {
    SOP_KEY_DOWN = 1,        // u8 vk                      KeyDown
    SOP_KEY_UP,              // u8 vk                      KeyUp
    SOP_KEY_CHAR,            // u8 ch, u8 scan             KeyQueueChar
    SOP_KEY_META,            // u8 vk, u8 scan             KeyQueueMetaChar
    SOP_MOUSE,               // u8 type, i16 x, i16 y, u8 buttons   MouseQueueEvent (in the game's pixels)
    SOP_ACTIVE,              // u8 active                  WM_ACTIVATEAPP: the inactive flag, the switch-away pause
    SOP_CLEAR_BITS,          //                            KeyClearBits (back from switched away)
    SOP_RESTORE,             //                            gxRestore
    SOP_SCAN,                // u8 n, n x u8 DIK codes     ScanUpdate: the keys held (DirectInput codes)
    SOP_QUIT,                //                            the window closed: ExitProcess
    SOP_NET,                 //                            a winsock reply (WSAAsyncGetHostByName's message): net_wsock.cpp
};
void session_idle_begin();
void session_op(uint8_t op, const void* p = 0, uint8_t n = 0);
void session_idle_end();
bool session_play_idle();
void platform_session_apply(uint8_t op, const uint8_t* p, uint8_t n);   // (platform.cpp)
// a switch away or back outside Win32Idle (recorded where it happens; a replay applies it at that point)
void session_active(bool active);

// ---- the race recorder (replay.cpp) -------------------------------------------------------------------------------------
uint32_t session_seed(uint32_t chosen);                        // Randomize: the seed (a replay's is the recording's)
bool session_random_feed(int range, int* v);                   // Random on the main thread: a replay's value
void session_random_saw(int range, int v);
// a race starting (on the physics thread, while the main thread waits in PhysicsStart / PhysicsRestart): true if the
// session owns it -- record it to dir\name.vpr, or (play) replay dir\name.vpr with this run's label
bool session_race_file(char* dir, size_t ndir, char* name, size_t nname, bool* play, char* label, size_t nlabel);
// the lockstep's gate, at the top of each physics update (the BG thread): false -- the update doesn't run (the main
// thread moved the task on while it waited); granted -- the main thread let it run: session_update_done() after it
bool session_update_gate(bool* granted);
void session_update_done();

// ---- the frame hash (gl_core.cpp) ---------------------------------------------------------------------------------------
// What the game hands the renderer, by event (SG_*), each hashed with the state changes since the event before it, into
// four parts (SP_*); the 2D page also into a 4 x 4 grid of tiles. session_frame() ends a frame (Flip).
enum : uint8_t { SP_PAGE, SP_SURFACES, SP_STATE, SP_DRAWS, SP_N };
enum : uint8_t {
    SG_PAGE = 1,             // the back buffer's 2D page, at Unlock
    SG_SURFACE,              // a surface made
    SG_UNLOCK,               // an offscreen / texture surface written (Unlock)
    SG_BLIT,                 // a blit (the destination rectangle after it)
    SG_TEXLOAD,              // IDirect3DTexture2::Load
    SG_KEY,                  // a colour key set
    SG_MODE,                 // the display mode
    SG_CLEAR,                // a viewport clear
    SG_TRANSFORM,            // TransformVertices (the game's input vertices)
    SG_DRAW,                 // a DrawPrimitive / DrawIndexedPrimitive
    SG_NET_SEND,             // a sendto (session.cpp, from net_wsock.cpp): its socket, destination and bytes
};
void session_gfx(uint8_t kind, const void* a, size_t na, const void* b = 0, size_t nb = 0, const void* c = 0, size_t nc = 0);
void session_gfx_state(uint32_t what, const void* p, size_t n);
void session_gfx_page(const uint16_t* page, int w, int h);
void session_frame();
uint64_t session_hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

// ---- the network (net_wsock.cpp; multiplayer stage N0) ------------------------------------------------------------------
// What the game reads from winsock, recorded per thread (the main thread, the lobby task, the physics thread) as records
// of these ops, and fed back on the same thread's channel in a replay, which makes no real socket call.
enum : uint8_t {
    NOP_STARTUP = 1, NOP_CLEANUP, NOP_SOCKET, NOP_BIND, NOP_SETSOCKOPT, NOP_IOCTL, NOP_GETSOCKOPT, NOP_CLOSE,
    NOP_RECVFROM, NOP_LASTERROR, NOP_HOSTNAME, NOP_HOSTBYNAME, NOP_INET_ADDR, NOP_ASYNC_HOST, NOP_ASYNC_CANCEL,
    NOP_ASYNC_REPLY, NOP_SENDTO, NOP_CLOCK, NOP_RANDOM,
    NOP_PARK,                // the lobby task, running free (its lockstep dropped), suspends here: its channel resyncs
    NOP_QPC,                 // the physics thread's clock (QueryPerformanceCounter) in a network race's physics task
};
bool session_net_live();                       // the real winsock is called (no session, or recording)
bool session_net_recording();
bool session_net_main();                       // on the session's main thread
bool session_net_lobby();                      // on the lobby task's thread
// a replay: the next record of `op` on this thread's channel (false: none -- the caller makes up a failure)
bool session_net_take(uint8_t op, const uint8_t** p, uint32_t* n);
void session_net_put(uint8_t op, const void* p, size_t n);        // recording: a value read
// a sendto: recorded (role = the socket's creation order), or compared with the recording's; hashed into the frame.
// Returns what the game gets back: live_ret, or (a replay) the recording's
int session_net_sent(uint8_t role, const void* to, int tolen, const void* buf, int len, int live_ret);
void session_net_async_event();                // recording: a winsock reply just arrived (an op of this Win32Idle)
void net_session_report();
// the lobby task (LiveMultiInfo::thread) in lockstep: its two sleeps and two self-suspensions, and Grab / Release
void session_lobby_sleep(int ms, void(__cdecl* real)(int));
void session_lobby_suspend_me(void(__cdecl* real)(void));
void session_lobby_grab(void* lmi, bool after);
void session_lobby_release(void* lmi, bool after);
// the physics task's entries in a network race (replay.cpp: PhysTaskBegin / Update / Restart / End, on the physics
// thread): between on and off every clock read there (QueryPerformanceCounter -- PTimeNow: TimerConditioner::GetTicks'
// resync base, PhysicsDeviation, PhysReplayAddEvent, the multiplayer code's) is recorded on the physics channel / fed
bool session_phys_clock(bool on);                 // returns the previous state, for
void session_phys_clock_restore(bool was);         // a nested entry (a restart from inside an update)
bool session_phys_clock_on();                  // (net_wsock.cpp: this thread's PTimeNow is already the session's)
