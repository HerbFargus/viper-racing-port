// net_multi.cpp -- multiplayer stage N2, group A: the multiplayer API and the live game's lock, rewritten faithfully (library
// `multi`: multi.obj).
//
// LiveMultiInfo (0x3c bytes, LiveMultiInfo::Create) is the live network game: the host's IRaceServer, the IRaceClient, the
// SessionMgr, and the lobby task (TaskCreate "Multiplayer" -> LiveMultiInfo::thread, 250 ms) that ticks them between races.
// Whoever uses them takes the lock first: Grab suspends the lobby task (waiting up to 3 s for it to really be suspended) and
// makes the caller the owner of the server, the client and the SessionMgr (SetOwner: their task-ownership checks); Release
// hands them back to the lobby task and resumes it; every member runs inside the critical section MultiBegin made
// (_MultiEnter / _MultiLeave). The public Multi* API: MultiLiveBegin / End (the global LiveMultiInfo, 0x4fb454: MultiEnabled),
// the physics thread's MultiBGRecv / MultiBGSend (inside lockstep: the client's state machine through the green flag --
// WorldLoaded, the deity's Update before the race, the green-flag time into Deity::StartRaceAt --, SessionMgr::Tick, the
// server's Tick and its RestartRace when MultiRestartRace asked, autoend_race 8 s after every car finished, the client's
// Tick; and SendAll), the main thread's MultiFGTick (the lobby took over: the "lost" screen and PhysicsAbortRace),
// MultiSynchronize (the pre-race wait: the car list, the track's name, the client's Synchronize, the server's
// EveryoneSynchronized), the car (un)registration, chat, the packet delay, MultiCarIsHuman, MultiDeityCast.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this file's functions too, so
// the hooked rewrite or the original is what runs), virtual calls through the vtable (IRaceClient's, IRaceServer's, the
// deity's), the statics read and written at the points the original reads and writes them. N0's patched sites (net_wsock.h)
// go through its entry points, as the patched originals do:
//   PTimeNow  0x4a23a0 (MultiTurtle), 0x4a2489 / 0x4a2519 (MultiBGRecv), 0x4a260d (autoend_race), 0x4a2c6c / 0x4a2c81 /
//             0x4a2ca7 (~LiveMultiInfo), 0x4a2d31 / 0x4a2d3c / 0x4a2d6a (Grab)       -> net_PTimeNow
//   Grab      0x4a2472 (MultiBGRecv), 0x4a276f (MultiRegisterLocalCar), 0x4a27af (MultiRegisterNetCar), 0x4a284a
//             (MultiResumeChat), 0x4a2865 (MultiSynchronize)                              -> net_Grab
//   Release   0x4a2395 (MultiLiveBegin), 0x4a25bd (MultiBGRecv), 0x4a2791 (MultiRegisterLocalCar), 0x4a27d1
//             (MultiRegisterNetCar)                                                       -> net_Release
//   the lobby task's gates: TaskSleep(250) 0x4a22cf / 0x4a231b -> net_lobby_sleep, TaskSuspendMe 0x4a22e0 / 0x4a232c ->
//             net_lobby_suspend_me (LiveMultiInfo::thread)
// Grab and Release themselves are what N0's h_Grab / h_Release wrap (they call 0x4a2d10 / 0x4a2e50, which jump here): the
// rewrites make no net_ call of their own, so the handover keeps the original's order (_MultiEnter, TaskSuspend, the wait,
// SetOwner, _MultiLeave; _MultiEnter, SetOwner, TaskResume, _MultiLeave).
//
// Footprints: the one-byte flags and the packet delay list their static; whatever sends, logs, ticks the session, takes or
// hands back the lock, creates or destroys tasks, allocates, or draws is replay_only (the session replay of a network game is
// its in-game check; test/world_net_client.cpp checks every one offline).
//
// FIX CANDIDATEs (left faithful): clear_screen loops forever if gxGrabScreen never succeeds (no screen to grab: 4 grabs are
// counted, not 4 tries); MultiSynchronize reads the proposal's track through GetProposal's result unchecked (0 if the client
// has none -- only in state 0xf, where the server has sent one); MultiAdjustPacketDelay's sum wraps (an int add, then the
// sign); MultiBGRecv / MultiFGTick / the car registration assume a client (LiveMultiInfo::Create always makes one).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_race.h"
#include "net_wsock.h"

namespace {
namespace net_multi {
using nt::Edx;
using nt::vcall;
using nt::tcall;
using nt::ccall;
using nt::crt_strcpy;
using namespace nr;

// this file's own functions, by address
enum : uint32_t {
    A_thread = 0x004a22c0, A_MultiEnabled = 0x004a23c0, A_autoend_race = 0x004a25d0, A_clear_screen = 0x004a2700,
    A_wait_for_key = 0x004a2a50, A_LMI_ctor = 0x004a2be0, A_LMI_dtor = 0x004a2c40, A_LMI_Tick = 0x004a2dc0,
    A_LMI_AmOwner = 0x004a2e10, A_LMI_SetOwner = 0x004a2e90, A_LMI_Ok = 0x004a2ec0,
    S_KEY_SYNCHRONIZING = 0x004fb4cc, S_KEY_WAITING = 0x004fb4e0, S_EMPTY = 0x004fb4f0, S_FMT_DELAY = 0x004fb4f4,
    S_LMI_NAME = 0x004fb528, S_TASK_NAME = 0x004fb538, S_GRAB_PANIC = 0x004fb544, S_DUMP = 0x004fb568,
    A_dtor_synchronizing = 0x004a2a70, A_dtor_waiting = 0x004a2a60,      // their atexit destructors ($E67 / $E66)
};

static __forceinline LiveMultiInfo* lmi() { return *(LiveMultiInfo* volatile*)(uintptr_t)G_LMI; }
static __forceinline LiveMultiInfo* lmi_thread() { return *(LiveMultiInfo* volatile*)(uintptr_t)G_LMI_THREAD; }
static __forceinline void* deity() { return *(void* volatile*)(uintptr_t)G_DEITY; }
static __forceinline int32_t client_state(const void* c) { return *(const volatile int32_t*)((const uint8_t*)c + 4); }
static __forceinline uint8_t multi_enabled() { return ccall<uint8_t>(A_MultiEnabled); }
static __forceinline uint8_t am_owner(LiveMultiInfo* m) { return tcall<uint8_t>(A_LMI_AmOwner, m); }
static __forceinline int32_t wrap_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static __forceinline int32_t wrap_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
// an Xlator's text, translated again if the language changed (its cookie isn't Xlator::g_cookie)
static __forceinline const char* xl_text(uint32_t x) {
    if (NT_GU32(x + 8) != NT_GU32(G_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (const void*)(uintptr_t)x);
    return (const char*)(uintptr_t)NT_GU32(x + 4);
}

static const char* const R_LOBBY = "ticks the session and the server / client (sends, receives)";
static const char* const R_LOCK = "takes or hands back the live game (suspends / resumes the lobby task)";
static const char* const R_TASK = "creates or destroys the lobby task, allocates or frees";
static const char* const R_UI = "draws and waits (the multiplayer screens)";
static const char* const R_VIA = "through the client's / server's vtable (they send, or write their own state)";
static void fp_flag(Footprint& f, uint32_t at, const char* what) { f.add((void*)(uintptr_t)at, 1, what); }

// =========================================================================================================================
// the lobby task
// =========================================================================================================================
static void __cdecl LMI_thread() {
    ccall<void>(F_TaskSetPriority, 15);
    net_lobby_sleep(250);                                               // site 0x4a22cf
    if (ccall<uint8_t>(F_TaskShouldISuspend)) net_lobby_suspend_me();   // site 0x4a22e0
    do {
        LiveMultiInfo* const m = lmi_thread();
        if (m->live) {
            tcall<void>(F_SM_Tick, m->sm);
            vcall<void>(lmi_thread()->client, RC_TICK);
            void* const s = lmi_thread()->server;
            if (s) vcall<void>(s, RS_TICK);
        }
        net_lobby_sleep(250);                                           // site 0x4a231b
        if (ccall<uint8_t>(F_TaskShouldISuspend)) net_lobby_suspend_me();   // site 0x4a232c
    } while (!ccall<uint8_t>(F_TaskShouldIDie));
}
static void fp_thread(Footprint& f) { f.replay_only = R_LOBBY; }
PORT_FN(0x004a22c0, "LiveMultiInfo::thread", LMI_thread, fp_thread)

// =========================================================================================================================
// the small API
// =========================================================================================================================
static void __cdecl MultiToggleMonitor_c() { NT_G8(G_MONITOR) = (uint8_t)(NT_G8(G_MONITOR) ^ 1); }
static void fp_toggle_monitor(Footprint& f) { fp_flag(f, G_MONITOR, "the monitor flag"); }
PORT_FN(0x004a2340, "MultiToggleMonitor", MultiToggleMonitor_c, fp_toggle_monitor)

static uint8_t __cdecl MultiMonitorIsEnabled_c() { return NT_G8(G_MONITOR); }
static void fp_none(Footprint&) {}
PORT_FN(0x004a2350, "MultiMonitorIsEnabled", MultiMonitorIsEnabled_c, fp_none)

static void __cdecl MultiLiveEnd_c() { NT_GU32(G_LMI) = 0; }
static void fp_live_end(Footprint& f) { f.add((void*)(uintptr_t)G_LMI, 4, "the live game"); }
PORT_FN(0x004a2360, "MultiLiveEnd", MultiLiveEnd_c, fp_live_end)

static void __cdecl MultiLiveBegin_c(LiveMultiInfo* m) {
    NT_GU32(G_LMI) = (uint32_t)(uintptr_t)m;
    NT_G8(G_ESCAPE) = 0;
    NT_G32(G_AUTOEND) = 0;
    NT_G8(G_PRE_RACE) = 0;
    NT_G8(G_TROUBLE) = 0;
    NT_G8(G_RESTART) = 0;
    net_Release(m, 0);                                                  // site 0x4a2395 (a tail jump)
}
static void fp_live_begin(Footprint& f, LiveMultiInfo*) { f.replay_only = R_LOCK; }
PORT_FN(0x004a2370, "MultiLiveBegin", MultiLiveBegin_c, fp_live_begin)

static uint8_t __cdecl MultiTurtle_c() {
    const int32_t now = net_PTimeNow();                                 // site 0x4a23a0
    return (uint8_t)(wrap_sub(now, NT_G32(G_TROUBLE_T)) < 250);
}
PORT_FN(0x004a23a0, "MultiTurtle", MultiTurtle_c, fp_none)

static uint8_t __cdecl MultiEnabled_c() { return (uint8_t)(lmi() != 0); }
PORT_FN(0x004a23c0, "MultiEnabled", MultiEnabled_c, fp_none)

static uint8_t __cdecl MultiIsServer_c() { return (uint8_t)(lmi()->server != 0); }
PORT_FN(0x004a23d0, "MultiIsServer", MultiIsServer_c, fp_none)

static void __cdecl MultiWorldIsLive_c() { NT_G8(G_WORLD_LIVE) = 1; }
static void fp_world_live(Footprint& f) { fp_flag(f, G_WORLD_LIVE, "the world-is-live flag"); }
PORT_FN(0x004a2420, "MultiWorldIsLive", MultiWorldIsLive_c, fp_world_live)

static void __cdecl MultiWorldIsDead_c() { NT_G8(G_WORLD_LIVE) = 0; }
PORT_FN(0x004a2430, "MultiWorldIsDead", MultiWorldIsDead_c, fp_world_live)

// =========================================================================================================================
// the physics thread's side (lockstep)
// =========================================================================================================================
typedef float(__cdecl* PTimeToPhys_t)(int);
static void __cdecl MultiBGRecv_c() {
    if (multi_enabled() && NT_G8(G_WORLD_LIVE)) {
        if (!am_owner(lmi())) net_Grab(lmi(), 0);                      // site 0x4a2472
        bool end = NT_G8(G_ESCAPE) != 0;
        if (!end && NT_G32(G_AUTOEND) != 0) {
            const int32_t now = net_PTimeNow();                         // site 0x4a2489
            end = now > NT_G32(G_AUTOEND);
        }
        if (end) {
            LiveMultiInfo* const m = lmi();
            NT_G8(G_ESCAPE) = 0;
            void* const s = m->server;
            if (s) {
                vcall<void>(s, RS_GO_BACK_TO_CHAT);
            } else {
                vcall<void>(lmi()->client, RC_DISCONNECT);
                NT_G8(G_TROUBLE) = 1;
            }
        }
        void* const c = lmi()->client;
        const int32_t st = client_state(c);
        if (st <= 4) {
            tcall<void>(A_LMI_Tick, lmi());
            NT_G8(G_PRE_RACE) = 1;
            return;
        }
        if (st == 0x12) {
            vcall<void>(c, RC_WORLD_LOADED);
        } else if (st <= 0xb) {
            vcall<void>(deity(), DT_UPDATE);
        } else if (st == 0x14) {
            int32_t t;
            vcall<void>(c, RC_GET_GREENFLAG_TIME, &t);
            if (ccall<int>(F_GetGameState) == 1) ccall<void>(F_PhysTaskRestart);
            const int32_t now = net_PTimeNow();                         // site 0x4a2519
            const float at = ((PTimeToPhys_t)(uintptr_t)F_ConvertPTimeToPhysicsTime)(wrap_add(t, now));
            vcall<void>(deity(), DT_START_RACE_AT, at);
        }
        tcall<void>(F_SM_Tick, lmi()->sm);
        void* const s = lmi()->server;
        if (s) {
            vcall<void>(s, RS_TICK);
            ccall<void>(A_autoend_race);
            if (NT_G8(G_RESTART)) {
                LiveMultiInfo* const m = lmi();
                NT_G8(G_RESTART) = 0;
                vcall<void>(m->server, RS_RESTART_RACE);
            }
        }
        vcall<void>(lmi()->client, RC_TICK);
        return;
    }
    if (lmi() && am_owner(lmi())) net_Release(lmi(), 0);                 // site 0x4a25bd
}
static void fp_bgrecv(Footprint& f) { f.replay_only = R_LOBBY; }
PORT_FN(0x004a2440, "MultiBGRecv", MultiBGRecv_c, fp_bgrecv)

static void __cdecl autoend_race_c() {
    if (NT_G32(G_AUTOEND) != 0) return;
    int32_t i = 0;
    const int32_t cars = ccall<int>(F_CarMgrCount);
    const int32_t n = cars - (int32_t)ccall<uint8_t>(F_GhostCarIncognito);
    if (n > 0) {
        do {
            if (!vcall<uint8_t>(deity(), DT_CAR_IS_FINISHED, i)) return;
            i++;
        } while (n > i);
    }
    NT_G32(G_AUTOEND) = wrap_add(net_PTimeNow(), 8000);                  // site 0x4a260d
}
static void fp_autoend(Footprint& f) { f.add((void*)(uintptr_t)G_AUTOEND, 4, "autoend_race's time"); }
PORT_FN(0x004a25d0, "autoend_race", autoend_race_c, fp_autoend)

static void __cdecl MultiBGSend_c() {
    if (!multi_enabled() || !NT_G8(G_WORLD_LIVE) || !am_owner(lmi())) return;
    vcall<void>(lmi()->client, RC_SEND_ALL);
    LiveMultiInfo* const m = lmi();
    if (!m->server) return;
    tcall<void>(F_SM_Tick, m->sm);
    vcall<void>(lmi()->server, RS_TICK);
    vcall<void>(lmi()->server, RS_SEND_ALL);                            // (a tail jump)
}
PORT_FN(0x004a2620, "MultiBGSend", MultiBGSend_c, fp_bgrecv)

// =========================================================================================================================
// the main thread's side
// =========================================================================================================================
static void __cdecl MultiFGTick_c() {
    if (!multi_enabled() || !NT_G8(G_PRE_RACE)) return;
    if (!NT_G8(G_TROUBLE)) {
        ccall<void>(A_clear_screen, xl_text(G_XL_DISCONNECTED));
        for (int i = 40; i; i--) {
            ccall<void>(F_TaskSleep, 50);
            ccall<void>(F_Win32Idle);
        }
    }
    ccall<void>(F_PhysicsAbortRace);
    ccall<void>(F_TaskSleep, 100);
}
static void fp_fgtick(Footprint& f) { f.replay_only = R_UI; }
PORT_FN(0x004a2690, "MultiFGTick", MultiFGTick_c, fp_fgtick)

static void __cdecl clear_screen_c(const char* text) {
    uint8_t canvas[0x24];                                               // a gxCanvas (made current: left aimed at this frame)
    int32_t n = 4;
    do {
        if (ccall<uint8_t>(F_gxGrabScreen, (void*)canvas)) {
            n--;
            ccall<void*>(F_gxSetCanvas, (void*)canvas);
            ccall<void>(F_gxClear, 0u);
            ccall<void>(F_gxReleaseScreen);
            ccall<void>(F_gxFlip);
        }
        ccall<void>(F_Win32Idle);
    } while (n);
    ccall<void>(F_SplashGeneric, text);
}
static void fp_clear_screen(Footprint& f, const char*) { f.replay_only = R_UI; }
PORT_FN(0x004a2700, "clear_screen(multi.obj)", clear_screen_c, fp_clear_screen)

static void __cdecl MultiRegisterLocalCar_c(void* car, int idx) {
    if (!multi_enabled()) return;
    net_Grab(lmi(), 0);                                                 // site 0x4a276f
    vcall<void>(lmi()->client, RC_REGISTER_LOCALCAR, car, idx);
    net_Release(lmi(), 0);                                              // site 0x4a2791 (a tail jump)
}
static void fp_register(Footprint& f, void*, int) { f.replay_only = R_LOCK; }
PORT_FN(0x004a2760, "MultiRegisterLocalCar", MultiRegisterLocalCar_c, fp_register)

static void __cdecl MultiRegisterNetCar_c(void* car, int idx) {
    if (!multi_enabled()) return;
    net_Grab(lmi(), 0);                                                 // site 0x4a27af
    vcall<void>(lmi()->client, RC_REGISTER_NETCAR, car, idx);
    net_Release(lmi(), 0);                                              // site 0x4a27d1 (a tail jump)
}
PORT_FN(0x004a27a0, "MultiRegisterNetCar", MultiRegisterNetCar_c, fp_register)

static void __cdecl MultiUnregisterLocalCar_c(void* car) {
    if (!multi_enabled()) return;
    vcall<void>(lmi()->client, RC_UNREGISTER, car);
}
static void fp_unregister(Footprint& f, void*) { f.replay_only = R_VIA; }
PORT_FN(0x004a27e0, "MultiUnregisterLocalCar", MultiUnregisterLocalCar_c, fp_unregister)

static void __cdecl MultiUnregisterNetCar_c(void* car) {
    if (!multi_enabled()) return;
    vcall<void>(lmi()->client, RC_UNREGISTER, car);
}
PORT_FN(0x004a2800, "MultiUnregisterNetCar", MultiUnregisterNetCar_c, fp_unregister)

static void __cdecl MultiRestartRace_c() { NT_G8(G_RESTART) = 1; }
static void fp_restart(Footprint& f) { fp_flag(f, G_RESTART, "the restart request"); }
PORT_FN(0x004a2820, "MultiRestartRace", MultiRestartRace_c, fp_restart)

static void __cdecl MultiResumeChat_c() {
    LiveMultiInfo* const m = lmi();
    if (m) net_Grab(m, 0);                                              // site 0x4a284a (a tail jump)
}
static void fp_resume_chat(Footprint& f) { f.replay_only = R_LOCK; }
PORT_FN(0x004a2840, "MultiResumeChat", MultiResumeChat_c, fp_resume_chat)

static uint8_t __cdecl MultiSynchronize_c(LiveMultiInfo* m, void* cars, char* track) {
    if (!am_owner(m)) net_Grab(m, 0);                                   // site 0x4a2865
    if (!(NT_G8(G_SYNC_GUARD) & 1)) {
        NT_G8(G_SYNC_GUARD) = (uint8_t)(NT_G8(G_SYNC_GUARD) | 1);
        tcall<void*>(F_Xlator_ctor, (const void*)(uintptr_t)G_XL_SYNCHRONIZING, NT_CP(S_KEY_SYNCHRONIZING));
        ccall<int>(F_atexit, (uint32_t)A_dtor_synchronizing);
    }
    if (!(NT_G8(G_SYNC_GUARD) & 2)) {
        NT_G8(G_SYNC_GUARD) = (uint8_t)(NT_G8(G_SYNC_GUARD) | 2);
        tcall<void*>(F_Xlator_ctor, (const void*)(uintptr_t)G_XL_WAITING, NT_CP(S_KEY_WAITING));
        ccall<int>(F_atexit, (uint32_t)A_dtor_waiting);
    }
    ccall<void>(A_clear_screen, xl_text(G_XL_SYNCHRONIZING));
    ccall<void>(F_TaskSetPriority, 2);
    int32_t st = client_state(m->client);
    if (st > 4) {
        for (;;) {
            if (st > 0x10) break;
            ccall<void>(F_Win32Idle);
            tcall<void>(A_LMI_Tick, m);
            void* const c = m->client;
            if (client_state(c) == 0xf) {
                vcall<void>(c, RC_GET_CAR_LIST, cars);
                const uint8_t* const prop = vcall<const uint8_t*>(m->client, RC_GET_PROPOSAL);
                const char* const name = ccall<const char*>(F_GetTrackName, *(const volatile int32_t*)(prop + 0x30));
                crt_strcpy(track, name);
                vcall<void>(m->client, RC_SYNCHRONIZE);
            }
            st = client_state(m->client);
            if (!(st > 4)) break;
        }
    }
    void* const s = m->server;
    if (s && !vcall<uint8_t>(s, RS_EVERYONE_SYNCHRONIZED)) {
        while (client_state(m->client) > 4) {
            ccall<void>(F_Win32Idle);
            tcall<void>(A_LMI_Tick, m);
            if (vcall<uint8_t>(m->server, RS_EVERYONE_SYNCHRONIZED)) break;
        }
    }
    ccall<void>(A_clear_screen, xl_text(G_XL_WAITING));
    if (st == 0x11) {                                                   // (the first loop's last state)
        do {
            ccall<void>(F_Win32Idle);
            tcall<void>(A_LMI_Tick, m);
        } while (client_state(m->client) == 0x11);
    }
    ccall<void>(A_clear_screen, NT_CP(S_EMPTY));
    ccall<void>(F_TaskSetPriority, 0);
    if (client_state(m->client) >= 4) return 1;
    NT_GU32(G_LMI) = (uint32_t)(uintptr_t)m;
    ccall<void>(A_clear_screen, xl_text(G_XL_DISCONNECTED));
    ccall<void>(A_wait_for_key);
    NT_GU32(G_LMI) = 0;
    return 0;
}
static void fp_synchronize(Footprint& f, LiveMultiInfo*, void*, char*) { f.replay_only = R_UI; }
PORT_FN(0x004a2850, "MultiSynchronize", MultiSynchronize_c, fp_synchronize)

static uint8_t __cdecl MultiMenuEscape_c() {
    NT_G8(G_ESCAPE) = 1;
    return 1;
}
static void fp_menu_escape(Footprint& f) { fp_flag(f, G_ESCAPE, "the escape request"); }
PORT_FN(0x004a2a80, "MultiMenuEscape", MultiMenuEscape_c, fp_menu_escape)

static void __cdecl MultiSendChat_c(const char* text) {
    if (!multi_enabled()) return;
    vcall<void>(lmi()->client, RC_SPEAK, text);
}
static void fp_send_chat(Footprint& f, const char*) { f.replay_only = R_VIA; }
PORT_FN(0x004a2a90, "MultiSendChat", MultiSendChat_c, fp_send_chat)

static void __cdecl MultiAdjustPacketDelay_c(int d) {
    if (!multi_enabled()) return;
    int32_t v = wrap_add(NT_G32(G_PACKET_DELAY), d);
    NT_G32(G_PACKET_DELAY) = v;
    if (v < 0) {
        v = 0;
    } else {
        NT_G32(G_PACKET_DELAY) = v;
        if (v > 2000) v = 2000;
    }
    NT_G32(G_PACKET_DELAY) = v;
    NT_LogReport(NT_CP(S_FMT_DELAY), v);
}
static void fp_adjust_delay(Footprint& f, int) { f.replay_only = "logs (LogReport)"; }
PORT_FN(0x004a2ab0, "MultiAdjustPacketDelay", MultiAdjustPacketDelay_c, fp_adjust_delay)

static int __cdecl MultiGetPacketDelay_c() { return NT_G32(G_PACKET_DELAY); }
PORT_FN(0x004a2b00, "MultiGetPacketDelay", MultiGetPacketDelay_c, fp_none)

static void __cdecl MultiSetPacketDelay_c(int d) { NT_G32(G_PACKET_DELAY) = d; }
static void fp_set_delay(Footprint& f, int) { f.add((void*)(uintptr_t)G_PACKET_DELAY, 4, "the packet delay"); }
PORT_FN(0x004a2b10, "MultiSetPacketDelay", MultiSetPacketDelay_c, fp_set_delay)

static const void* __cdecl MultiGetRaceInfo_c() { return vcall<const void*>(lmi()->client, RC_GET_RACE_INFO); }
PORT_FN(0x004a2b20, "MultiGetRaceInfo", MultiGetRaceInfo_c, fp_none)

static uint8_t __cdecl MultiCarIsHuman_c(int car) {
    if (!lmi()) return 0;
    void* const c = lmi()->client;
    if (client_state(c) <= 4) return 0;
    return vcall<uint8_t>(c, RC_CAR_IS_HUMAN, car);
}
static void fp_car_is_human(Footprint& f, int) { f.replay_only = "the client's CarIsHuman (its state assert translates)"; }
PORT_FN(0x004a2b30, "MultiCarIsHuman", MultiCarIsHuman_c, fp_car_is_human)

static void __cdecl MultiConsolidateRecords_c() { multi_enabled(); }      // (a tail jump to MultiEnabled)
PORT_FN(0x004a2b60, "MultiConsolidateRecords", MultiConsolidateRecords_c, fp_none)

static void __cdecl MultiDeityCast_c(const void* pkt, int len) { vcall<void>(lmi()->client, RC_DEITY_CAST, pkt, len); }
static void fp_deity_cast(Footprint& f, const void*, int) { f.replay_only = R_VIA; }
PORT_FN(0x004a2b70, "MultiDeityCast", MultiDeityCast_c, fp_deity_cast)

// =========================================================================================================================
// LiveMultiInfo
// =========================================================================================================================
static LiveMultiInfo* __cdecl LMI_Create() {
    LiveMultiInfo* r = 0;
    void* const p = ccall<void*>(F_MemAlloc, 0x3c);
    if (p) r = tcall<LiveMultiInfo*>(A_LMI_ctor, p);
    if (r && !tcall<uint8_t>(A_LMI_Ok, r)) {
        tcall<void>(A_LMI_dtor, r);
        ccall<void>(F_op_delete, r);
        r = 0;
    }
    return r;
}
static void fp_lmi_create(Footprint& f) { f.replay_only = R_TASK; }
PORT_FN(0x004a2b90, "LiveMultiInfo::Create", LMI_Create, fp_lmi_create)

static LiveMultiInfo* __fastcall LMI_ctor(LiveMultiInfo* self, Edx) {
    self->multi = ccall<int>(F_MultiBegin, NT_CP(S_LMI_NAME));
    self->live = 0;
    NT_GU32(G_LMI_THREAD) = (uint32_t)(uintptr_t)self;
    self->server = 0;
    self->client = 0;
    self->sm = 0;
    self->_10 = 0;
    const int task = ccall<int>(F_TaskCreate, NT_CP(S_TASK_NAME), (uint32_t)A_thread);
    self->task = task;
    ccall<void>(F_TaskSuspend, task);
    self->owner = self->task;
    return self;
}
static void fp_lmi_ctor(Footprint& f, LiveMultiInfo*, Edx) { f.replay_only = R_TASK; }
PORT_FN(0x004a2be0, "LiveMultiInfo::LiveMultiInfo", LMI_ctor, fp_lmi_ctor)

static void __fastcall LMI_dtor(LiveMultiInfo* self, Edx) {
    self->live = 0;
    self->_10 = 0;
    if (self->server) vcall<void*>(self->server, RS_DTOR, 1u);
    if (self->client) vcall<void*>(self->client, RC_DTOR, 1u);
    if (self->sm) {
        const int32_t until = wrap_add(net_PTimeNow(), 3000);           // site 0x4a2c6c
        tcall<void>(F_SM_Shutdown, self->sm);
        if (wrap_sub(until, net_PTimeNow()) > 0) {                      // site 0x4a2c81
            do {
                if (tcall<uint8_t>(F_SM_CanDestroySafely, self->sm)) break;
                tcall<void>(F_SM_Tick, self->sm);
                ccall<void>(F_Win32Idle);
            } while (wrap_sub(until, net_PTimeNow()) > 0);              // site 0x4a2ca7
        }
        netc::SessionMgr* const sm = self->sm;
        if (sm) {
            tcall<void>(F_SM_dtor, sm);
            ccall<void>(F_op_delete, sm);
        }
    }
    if (self->task) ccall<void>(F_TaskDestroy, self->task);
    self->owner = 0;
    self->task = 0;
    self->server = 0;
    self->client = 0;
    self->sm = 0;
    NT_GU32(G_LMI_THREAD) = 0;
    ccall<void>(F_MultiEnd, self->multi, 0u, 0u);
}
static void fp_lmi_dtor(Footprint& f, LiveMultiInfo*, Edx) { f.replay_only = R_TASK; }
PORT_FN(0x004a2c40, "LiveMultiInfo::~LiveMultiInfo", LMI_dtor, fp_lmi_dtor)

static void __fastcall LMI_Grab(LiveMultiInfo* self, Edx) {
    ccall<void>(F_MultiEnter, self->multi, 0u, 0u);
    ccall<void>(F_TaskSuspend, self->task);
    const int32_t until = wrap_add(net_PTimeNow(), 3000);               // site 0x4a2d31
    if (wrap_sub(until, net_PTimeNow()) > 0) {                          // site 0x4a2d3c
        do {
            if (ccall<uint8_t>(F_TaskIsSuspended, self->task)) break;
            ccall<void>(F_TaskSleep, 25);
            ccall<void>(F_Win32Idle);
        } while (wrap_sub(until, net_PTimeNow()) > 0);                  // site 0x4a2d6a
    }
    if (!ccall<uint8_t>(F_TaskIsSuspended, self->task)) NT_LogPanic(NT_CP(S_GRAB_PANIC));
    const int id = ccall<int>(F_TaskGetID);
    tcall<void>(A_LMI_SetOwner, self, id);
    ccall<void>(F_MultiLeave, self->multi, 0u, 0u);
}
static void fp_lmi_lock(Footprint& f, LiveMultiInfo*, Edx) { f.replay_only = R_LOCK; }
PORT_FN(0x004a2d10, "LiveMultiInfo::Grab", LMI_Grab, fp_lmi_lock)

static void __fastcall LMI_Tick(LiveMultiInfo* self, Edx) {
    ccall<void>(F_MultiEnter, self->multi, 0u, 0u);
    tcall<void>(F_SM_Tick, self->sm);
    if (self->server) vcall<void>(self->server, RS_TICK);
    vcall<void>(self->client, RC_TICK);
    ccall<void>(F_MultiLeave, self->multi, 0u, 0u);
}
static void fp_lmi_tick(Footprint& f, LiveMultiInfo*, Edx) { f.replay_only = R_LOBBY; }
PORT_FN(0x004a2dc0, "LiveMultiInfo::Tick", LMI_Tick, fp_lmi_tick)

static uint8_t __fastcall LMI_AmOwner(LiveMultiInfo* self, Edx) {
    ccall<void>(F_MultiEnter, self->multi, 0u, 0u);
    const int id = ccall<int>(F_TaskGetID);
    const uint8_t r = (uint8_t)(id == self->owner);
    ccall<void>(F_MultiLeave, self->multi, 0u, 0u);
    return r;
}
static void fp_lmi_read(Footprint&, LiveMultiInfo*, Edx) {}
PORT_FN(0x004a2e10, "LiveMultiInfo::AmOwner", LMI_AmOwner, fp_lmi_read)

static void __fastcall LMI_Release(LiveMultiInfo* self, Edx) {
    ccall<void>(F_MultiEnter, self->multi, 0u, 0u);
    tcall<void>(A_LMI_SetOwner, self, self->task);
    ccall<void>(F_TaskResume, self->task);
    ccall<void>(F_MultiLeave, self->multi, 0u, 0u);
}
PORT_FN(0x004a2e50, "LiveMultiInfo::Release", LMI_Release, fp_lmi_lock)

static void __fastcall LMI_SetOwner(LiveMultiInfo* self, Edx, int id) {
    void* const s = self->server;
    self->owner = id;
    if (s) vcall<void>(s, RS_SET_OWNER, id);
    vcall<void>(self->client, RC_SET_OWNER, id);
    self->sm->task = id;
}
static void fp_lmi_set_owner(Footprint& f, LiveMultiInfo*, Edx, int) { f.replay_only = R_VIA; }
PORT_FN(0x004a2e90, "LiveMultiInfo::SetOwner", LMI_SetOwner, fp_lmi_set_owner)

static uint8_t __fastcall LMI_Ok(LiveMultiInfo* self, Edx) { return (uint8_t)(self->task != 0); }
PORT_FN(0x004a2ec0, "LiveMultiInfo::Ok", LMI_Ok, fp_lmi_read)

static uint8_t __fastcall LMI_InUse(LiveMultiInfo* self, Edx) {
    ccall<void>(F_MultiEnter, self->multi, 0u, 0u);
    const uint8_t r = (uint8_t)(self->task != self->owner);
    ccall<void>(F_MultiLeave, self->multi, 0u, 0u);
    return r;
}
PORT_FN(0x004a2ed0, "LiveMultiInfo::InUse", LMI_InUse, fp_lmi_read)

static void __fastcall LMI_dump(LiveMultiInfo* self, Edx) {
    const int owner = self->owner, task = self->task;
    NT_LogReport(NT_CP(S_DUMP), ccall<int>(F_TaskGetID), task, owner);
}
static void fp_lmi_dump(Footprint& f, LiveMultiInfo*, Edx) { f.replay_only = "logs (LogReport)"; }
PORT_FN(0x004a2f10, "LiveMultiInfo::dump", LMI_dump, fp_lmi_dump)

}  // namespace net_multi
}  // namespace
