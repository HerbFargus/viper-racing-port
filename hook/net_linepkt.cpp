// net_linepkt.cpp -- multiplayer stage N1, group A: the serial and modem line's framing and line check, rewritten
// (library `multi`: linepkt.obj, linechek.obj, crc.obj, tapidbg.obj).
//
//   LinePacketizer: frames packets on an ILineDevice (a COM port or a modem, line.obj). A packet goes as a 6-byte header
//   -- three sync bytes 0xf0, its length (one byte: under 0xe2) and the CRC16 of its body -- then the body. Recv is a
//   state machine over what the device has: sync (count the 0xf0s, the last three bytes read), header, body (checked
//   against the CRC: a bad one is dropped quietly). It returns at most one packet per call.
//   LineChecker: before a line game the two ends ping each other with "(n)" every 500 ms (Tick) and count the replies that
//   follow on; LineOk once both counts pass 3; the end with the lower number is the client (IsServer).
//   CRC16 (crc.obj): the table-driven CCITT CRC (table at 0x4df358), also used by the AI's driving notes (phys_ai.cpp).
//   tapidbg.obj: the TAPI name tables of the debug build -- each returns one empty string here.
//
// Written from the v1.0 disassembly: every call in the original's order, by its v1.0 address (this group's own functions
// too, so a hooked rewrite or the original is what runs) or through the line device's vtable as the original makes it.
// The clock and the random number LineChecker reads are N0's patched call sites (net_wsock.cpp k_ptimenow_sites
// 0x4ae9bf / 0x4aea37 / 0x4aec47, k_random_sites 0x4ae9cf): the rewrites call net_PTimeNow / net_Random, which do what
// the patched site does in a session and call PTimeNow / Random otherwise.
//
// Footprints: CRC16, the constructor, GetHeaderSize, LineOk and the tapidbg strings are pure. Whatever reads or writes
// the line (LinePacketizer Send / Recv, LineChecker::Tick), reads the clock (LineChecker's constructor, DoneChecking) or
// frees the device (the packetizer's destructor) is replay_only: a session replay of a line game is their in-game check
// (no line hardware here), and test/world_net_line.cpp checks them all offline against the originals on a fake line.
//
// Also here: the bare `ret`s VERBOSE (linechek.obj) and tapidbg's dump_callstatedetail, dump_modemsettings and
// dump_devstatus. Left to tools/gen_leftovers.py --lib multi (group B's net_leftover.cpp): these objects' $E initialisers.
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_wsock.h"

namespace {
namespace net_linepkt {
using namespace nt;

// =========================================================================================================================
// crc.obj
// =========================================================================================================================
// CRC16(p, n): crc = table[(byte ^ crc >> 8) & 0xff] ^ crc << 8 over n bytes (none if n <= 0), from 0
static uint16_t __cdecl CRC16_c(const void* p, int32_t n) {
    enum : uint32_t { TABLE = 0x004df358 };
    const volatile uint8_t* s = (const volatile uint8_t*)p;
    uint32_t crc = 0;
    for (int32_t left = n; left > 0; left--) {
        const uint32_t b = (uint32_t)(int32_t)(int8_t)*s++;
        crc = (uint32_t)NT_G16(TABLE + (((b ^ (crc >> 8 & 0xff)) & 0xff) * 2)) ^ (crc << 8 & 0xffff);
    }
    return (uint16_t)crc;
}
static void fp_pure_cdecl_crc(Footprint& f, const void*, int32_t) { f.pure = true; }
PORT_FN(0x004a3d70, "CRC16", CRC16_c, fp_pure_cdecl_crc)

// =========================================================================================================================
// linepkt.obj
// =========================================================================================================================
static LinePacketizer* __fastcall LinePacketizer_ctor(LinePacketizer* self, Edx, void* dev) {
    self->dev = dev;
    self->state = 0;
    self->count = 0;
    return self;
}
static void fp_LinePacketizer_ctor(Footprint& f, LinePacketizer* self, Edx, void*) {
    // (not pure: it returns this, which the fuzzer would compare as a value)
    f.add(self, sizeof(LinePacketizer), "the packetizer");
}
PORT_FN(0x004af7d0, "LinePacketizer::LinePacketizer", LinePacketizer_ctor, fp_LinePacketizer_ctor)

// the destructor deletes its device (its deleting destructor, slot 8) and leaves state 3 (a Recv after it panics)
static void __fastcall LinePacketizer_dtor(LinePacketizer* self, Edx) {
    void* dev = self->dev;
    if (dev) vcall<void*>(dev, LD_DTOR, 1u);
    self->state = 3;
    self->dev = 0;
}
static void fp_LinePacketizer_dtor(Footprint& f, LinePacketizer*, Edx) { f.replay_only = "deletes its line device"; }
PORT_FN(0x004af7f0, "LinePacketizer::~LinePacketizer", LinePacketizer_dtor, fp_LinePacketizer_dtor)

// Send(p, n): the header {f0 f0 f0, n, CRC16(p, n)} (6 bytes of an 8-byte local), then the body, each with the device's Send
static void __fastcall LinePacketizer_Send(LinePacketizer* self, Edx, const void* p, int32_t n) {
    uint8_t h[8];
    h[0] = 0xf0;
    h[1] = 0xf0;
    h[2] = 0xf0;
    h[3] = (uint8_t)n;
    h[4] = 0;
    h[5] = 0;
    const uint16_t crc = ccall<uint16_t>(A_CRC16, p, n);
    memcpy(h + 4, &crc, 2);
    vcall<void>(self->dev, LD_SEND, (const void*)h, (int32_t)6);
    vcall<void>(self->dev, LD_SEND, p, n);
}
static void fp_LinePacketizer_Send(Footprint& f, LinePacketizer*, Edx, const void*, int32_t) { f.replay_only = "sends on the line"; }
PORT_FN(0x004af810, "LinePacketizer::Send", LinePacketizer_Send, fp_LinePacketizer_Send)

static int32_t __fastcall LinePacketizer_GetHeaderSize(LinePacketizer*, Edx) { return 8; }
template <typename T> static void fp_pure_this(Footprint& f, T*, Edx) { f.pure = true; }
PORT_FN(0x004af870, "LinePacketizer::GetHeaderSize", LinePacketizer_GetHeaderSize, fp_pure_this<LinePacketizer>)

// Recv(buf, &len): what the device has (DataAvail, once) is worked through the states until one makes no progress:
//   0 sync: read the 3 - count bytes still wanted into a zeroed 3-byte buffer; trailing 0xf0s are dropped from the count
//     read; while some other byte is left, read that many again. A read of nothing, or all 0xf0s: on to the header.
//     count = 3 - what was still wanted when it stopped.
//   1 header (3 bytes once there are 3): the length (under 0xe2, else logged and back to sync) and the CRC;
//   2 body: longer than the caller's buffer -> logged, back to sync; not all there -> wait; else read it (into the
//     length field: the device's count), back to sync, and a packet if the CRC matches;
//   3 (after the destructor) panics.
static uint8_t __fastcall LinePacketizer_Recv(LinePacketizer* self, Edx, void* buf, int32_t* len) {
    int32_t avail = vcall<int32_t>(self->dev, LD_DATA_AVAIL);
    uint8_t sync[3];
    int32_t want;
    uint8_t hdr[3];
    int32_t hn;
    uint8_t progress;
    do {
        progress = 0;
        switch ((int32_t)self->state) {
        case 0:
            want = 3 - (int32_t)self->count;
            sync[0] = 0;
            sync[1] = 0;
            sync[2] = 0;
            for (;;) {
                if (want > avail) break;
                vcall<uint8_t>(self->dev, LD_RECV, (void*)sync, &want);
                avail -= want;
                if (want != 0) {
                    while (sync[want - 1] == 0xf0)
                        if (--want == 0) break;
                    if (want != 0) continue;
                }
                self->state = 1;
                progress = 1;
                break;
            }
            self->count = (uint8_t)(3 - (uint8_t)want);
            break;
        case 1:
            if (avail < 3) break;
            hn = 3;
            avail -= 3;
            vcall<uint8_t>(self->dev, LD_RECV, (void*)hdr, &hn);
            if (hdr[0] < 0xe2) {
                self->state = 2;
                progress = 1;
                memcpy(&self->crc, hdr + 1, 2);
                self->len = hdr[0];
            } else {
                self->state = 0;
                self->count = 0;
                NT_LogReport(NT_CP(0x004fe6ac), (uint32_t)hdr[0], (uint32_t)0xe2);
                progress = 1;
            }
            break;
        case 2: {
            const int32_t n = self->len, cap = *len;
            if (n > cap) {
                NT_LogReport(NT_CP(0x004fe6d8), cap, n);
                self->state = 0;
                self->count = 0;
                break;
            }
            if (n > avail) break;
            vcall<uint8_t>(self->dev, LD_RECV, buf, &self->len);
            self->state = 0;
            self->count = 0;
            if (ccall<uint16_t>(A_CRC16, (const void*)buf, self->len) != self->crc) break;
            *len = self->len;
            return 1;
        }
        case 3:
            NT_LogPanic(NT_CP(0x004fe710));
            break;
        default:
            break;
        }
    } while (progress);
    return 0;
}
static void fp_LinePacketizer_Recv(Footprint& f, LinePacketizer*, Edx, void*, int32_t*) { f.replay_only = "reads the line"; }
PORT_FN(0x004af880, "LinePacketizer::Recv", LinePacketizer_Recv, fp_LinePacketizer_Recv)

// =========================================================================================================================
// linechek.obj
// =========================================================================================================================
// the time the check gives up: start + 7 s + 500 ms per bad reply ((extra * 4 + 0x38) * 125, 32-bit)
static __forceinline int32_t check_deadline(const LineChecker* c) {
    return (int32_t)(((uint32_t)c->extra * 4u + 0x38u) * 5u * 5u * 5u + (uint32_t)c->start);
}

static LineChecker* __fastcall LineChecker_ctor(LineChecker* self, Edx, void* dev) {
    self->dev = dev;
    self->peer_seq = 0;
    const int32_t now = net_PTimeNow();             // N0's site 0x4ae9bf
    self->start = now;
    self->next_send = now;
    self->my_seq = (uint32_t)net_Random(100000);    // N0's site 0x4ae9cf
    self->in_seq = 0;
    self->got = 0;
    self->sent = 0;
    self->extra = 0;
    self->toggle = 0;
    return self;
}
static void fp_LineChecker_ctor(Footprint& f, LineChecker*, Edx, void*) { f.replay_only = "reads the clock (PTimeNow)"; }
PORT_FN(0x004ae9b0, "LineChecker::LineChecker", LineChecker_ctor, fp_LineChecker_ctor)

static uint8_t __fastcall LineChecker_LineOk(LineChecker* self, Edx) {
    if (self->got <= 3) return 0;
    if (self->in_seq <= 3) return 0;
    return 1;
}
PORT_FN(0x004ae9f0, "LineChecker::LineOk", LineChecker_LineOk, fp_pure_this<LineChecker>)

// Tick: on a connected line, until DoneChecking, every 500 ms: send "(my_seq)" (with its NUL); every other time also read
// up to 0x40 bytes and parse "(n)": the first reply sets the peer's number; a later one that is the last plus one counts
// as in sequence, otherwise the start moves on 500 ms; a reply without "(...)" counts as bad (at most 40), and no reply
// at all moves the start on 500 ms. The buffer isn't terminated by the read (strchr runs on to a NUL).
static void __fastcall LineChecker_Tick(LineChecker* self, Edx) {
    char buf[0x40];
    int32_t n;
    if (vcall<int32_t>(self->dev, LD_GET_STATUS) != 2) return;
    if (tcall<uint8_t>(A_LineChecker_DoneChecking, self)) return;
    const int32_t now = net_PTimeNow();             // N0's site 0x4aea37
    const int32_t t = self->next_send;
    if (t >= now) return;
    self->next_send = t + 500;
    NT_sprintf(buf, NT_CP(0x004fe570), self->my_seq);
    vcall<void>(self->dev, LD_SEND, (const void*)buf, (int32_t)(crt_strlen(buf) + 1));
    self->sent++;
    n = 0x40;
    const uint8_t tg = (uint8_t)(self->toggle ^ 1);
    self->toggle = tg;
    if (tg == 0) return;
    if (!vcall<uint8_t>(self->dev, LD_RECV, (void*)buf, &n)) {
        NT_VERBOSE(NT_CP(0x004fe5c8), now, check_deadline(self));
        self->start += 500;
        return;
    }
    char* open = ccall<char*>(F_strchr, (const char*)buf, (int32_t)'(');
    char* close = ccall<char*>(F_strchr, (const char*)buf, (int32_t)')');
    if (open && close && (uintptr_t)close > (uintptr_t)open) {
        if (self->peer_seq == 0) {
            self->peer_seq = ccall<uint32_t>(F_strtoul, (const char*)(open + 1), (char**)0, (int32_t)10);
            self->got++;
            self->my_seq++;
            return;
        }
        const uint32_t v = ccall<uint32_t>(F_strtoul, (const char*)(open + 1), (char**)0, (int32_t)10);
        if (self->peer_seq - v == 0xffffffffu) {
            self->in_seq++;
            NT_VERBOSE(NT_CP(0x004fe578), now, check_deadline(self));
        } else {
            NT_VERBOSE(NT_CP(0x004fe588), now, check_deadline(self));
            self->start += 500;
        }
        self->peer_seq = v;
        self->got++;
        self->my_seq++;
        return;
    }
    const int32_t b2 = (int8_t)buf[2], b1 = (int8_t)buf[1], b0 = (int8_t)buf[0];
    const int32_t ordered = (uintptr_t)close > (uintptr_t)open ? 1 : 0;
    const int32_t x = self->extra;
    NT_VERBOSE(NT_CP(0x004fe5a0), now, x, check_deadline(self), open, close, ordered, b0, b1, b2);
    const int32_t e = self->extra;
    if (e >= 0x28) return;
    self->extra = e + 1;
}
static void fp_LineChecker_Tick(Footprint& f, LineChecker*, Edx) { f.replay_only = "talks on the line, reads the clock"; }
PORT_FN(0x004aea10, "LineChecker::Tick", LineChecker_Tick, fp_LineChecker_Tick)

static uint8_t __fastcall LineChecker_DoneChecking(LineChecker* self, Edx) {
    const int32_t deadline = check_deadline(self);
    return deadline < net_PTimeNow() ? 1 : 0;       // N0's site 0x4aec47
}
static void fp_LineChecker_DoneChecking(Footprint& f, LineChecker*, Edx) { f.replay_only = "reads the clock (PTimeNow)"; }
PORT_FN(0x004aec30, "LineChecker::DoneChecking", LineChecker_DoneChecking, fp_LineChecker_DoneChecking)

// IsServer: the lower number is the client (equal numbers are logged, and make both ends clients)
static uint8_t __fastcall LineChecker_IsServer(LineChecker* self, Edx) {
    if (self->my_seq == self->peer_seq) NT_LogReport(NT_CP(0x004fe5dc));
    return self->my_seq < self->peer_seq ? 1 : 0;
}
static void fp_LineChecker_IsServer(Footprint& f, LineChecker* self, Edx) {
    if (self->my_seq == self->peer_seq) f.replay_only = "logs (the two numbers are equal)";
}
PORT_FN(0x004aec60, "LineChecker::IsServer", LineChecker_IsServer, fp_LineChecker_IsServer)

// =========================================================================================================================
// tapidbg.obj: the release build's name tables are empty strings
// =========================================================================================================================
static const char* __cdecl tapierr_c(uint32_t) { return NT_CP(0x004fe5f0); }
static const char* __cdecl tapimsg2str_c(uint32_t) { return NT_CP(0x004fe5f4); }
static const char* __cdecl tapicallstate2str_c(uint32_t) { return NT_CP(0x004fe5f8); }
static const char* __cdecl pst_type_c(int32_t) { return NT_CP(0x004fe5fc); }
static void fp_pure_u32(Footprint& f, uint32_t) { f.pure = true; }
static void fp_pure_i32(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x004aecb0, "tapierr", tapierr_c, fp_pure_u32)
PORT_FN(0x004aecc0, "tapimsg2str", tapimsg2str_c, fp_pure_u32)
PORT_FN(0x004aecd0, "tapicallstate2str", tapicallstate2str_c, fp_pure_u32)
PORT_FN(0x004aece0, "pst_type", pst_type_c, fp_pure_i32)

// the debug build's dumps, bare `ret`s here
static void __cdecl VERBOSE_c(const char*) {}                    // (variadic: the caller pops what it pushed)
static void fp_verbose(Footprint& f, const char*) { f.pure = true; }
PORT_FN(0x004aec20, "VERBOSE(linechek.obj)", VERBOSE_c, fp_verbose)
static void __cdecl dump_callstatedetail_c(uint32_t, uint32_t) {}
static void fp_dump2(Footprint& f, uint32_t, uint32_t) { f.pure = true; }
PORT_FN(0x004aecf0, "dump_callstatedetail", dump_callstatedetail_c, fp_dump2)
static void __cdecl dump_modemsettings_c(const void*) {}
static void fp_dump1(Footprint& f, const void*) { f.pure = true; }
PORT_FN(0x004aed00, "dump_modemsettings", dump_modemsettings_c, fp_dump1)
static void __cdecl dump_devstatus_c(const void*) {}
PORT_FN(0x004aed10, "dump_devstatus", dump_devstatus_c, fp_dump1)

}  // namespace net_linepkt
}  // namespace
