// net_wsock.h -- multiplayer stage N0: the game's 18 wsock32 imports through one layer (net_wsock.cpp).
//
// Off unless a session records or replays ([session], session.cpp) or viperport.ini has `[test] two_copies=1`; then
// race.exe's wsock32 import slots point at net_wsock.cpp's wrappers. Off the recorder they pass straight through, with
// two test-only changes for two copies on one PC: copy 2 binds port 2002 instead of 2001, and copy 1's broadcasts to
// :2001 also go to :2002.
#pragma once

void net_install(const char* ini);   // session_install calls it, once the session's mode is decided
void net_play_async();
// a packet the game sends: m (0xff-filled by the caller) gets 0 for every bit the game never writes (stack garbage), by
// packet kind -- what a session compares and hashes is the packet ANDed with it (see net_wsock.cpp for the evidence)
#include <stdint.h>
void net_send_mask(const uint8_t* p, int n, uint8_t* m);               // a replay: the recording's next WSAAsyncGetHostByName reply, handed to the game

// ---- the multiplayer code's patched call sites, for its rewrites (stage N1 on) ----------------------------------------------
// N0 patches calls inside the original multiplayer code (net_install, v1.0, a session only). A rewrite of a function that
// contains such a site makes the SAME call through these: in a session whose sites were patched they do exactly what the
// patched site does (h_PTimeNow, h_Random, ...); otherwise (no session, another build, the site left stock) they pass
// straight through to the game's own function, as the stock call does.
int __cdecl net_PTimeNow();                       // PTimeNow (0x413b40), at k_ptimenow_sites
int __cdecl net_Random(int range);                // Random(int) (0x41b6e0), at k_random_sites
void __fastcall net_Grab(void* lmi, void* edx);   // LiveMultiInfo::Grab (0x4a2d10, __thiscall), at k_grab_sites
void __fastcall net_Release(void* lmi, void* edx);// LiveMultiInfo::Release (0x4a2e50, __thiscall), at k_release_sites
void __cdecl net_lobby_sleep(int ms);             // LiveMultiInfo::thread's TaskSleep(250)s (0x414de0), k_sleep_sites
void __cdecl net_lobby_suspend_me();              // and its TaskSuspendMe()s (0x414df0), k_suspend_sites
// winsock_grab's `push async_msg_hook` (PUSH_ASYNC_HOOK): the address to push -- N0's h_async_hook when it patched that
// push, else async_msg_hook (0x4adc00) itself
uint32_t net_async_hook_for_winsock_grab();
