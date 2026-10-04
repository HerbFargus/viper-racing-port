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
