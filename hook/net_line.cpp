// net_line.cpp -- multiplayer stage N1, group A: the serial and modem lines, rewritten (library `multi`: line.obj).
//
//   ILineDevice / LineDevice: a byte stream on a COM port. Send copies into one of 16 overlapped send blocks (WriteFile,
//   completion polled by free_completed_sendblocks); Recv is a plain ReadFile of what ClearCommError says is waiting
//   (DataAvail). The status (LineDeviceStatus, 0..11, 2 = connected) drives the line game's screens (LineStatusText).
//   DirectLine: a null-modem cable on COM1..4 at 19200 baud (8N1, RTS/CTS); "connected" while the other end's DCD (RLSD)
//   is up. TAPILine: a modem through TAPI 1.4 -- lineOpen for data-modem calls, the port handle from lineGetID
//   ("comm/datamodem"), the modem's settings from a fixed 0x6c-byte device configuration (set_from_bytestream), Call
//   (lineMakeCall 9600..19200) / Answer (an offered call) / Reset (drop and deallocate), driven by TAPI's callback
//   (tapi_cb -> TAPILine::message -> update_callstate). LineBegin / LineEnd start and stop TAPI and the send blocks'
//   events; LineEnumerateDevices lists the TAPI modems that are data modems, then the COM ports a modem doesn't use.
//
// Written from the v1.0 disassembly: every call in the original's order, by its v1.0 address (line.obj's own functions
// too, so a hooked rewrite or the original is what runs) or through the vtable as the original makes it (a vtable or an
// import slot the original keeps in a register across calls is read once, as there). KERNEL32, ADVAPI32 and TAPI32 are
// called through race.exe's own import slots, read at the call; the TAPI callback handed to lineInitialize is tapi_cb's
// v1.0 address (its rewrite, when hooked, is what TAPI calls). The function-local Xlators (LineDeviceInfo::GetError,
// LineStatusText) are built the first time as the original builds them (guard bit, constructor, atexit). Quirks kept:
// enum_devices zeroes its count after the TAPI pass of a "modems only" listing (flags 2), so the COM ports' pass
// overwrites it; LineStatusText's entries 8 and 10 are the same text.
//
// Fixes (docs/FIXES.md, "Multiplayer"; each marked `// FIX:` in place, `VP_FIX &&`, so the faithful build is the original):
// LineDevice::Send drops a packet over its send block's 0xe4 bytes; set_from_bytestream / TAPILine::get_handle /
// get_tapiline_port keep the offsets and sizes the TAPI driver hands back inside their buffers, and get_tapiline_port
// closes its registry key; lds_str and LineStatusText give their entry 0 for a status outside 0..11; enum_devices walks
// only its two lists; enumerate_tapi_devices clears COM4's "a modem's port" flag with the others; LineEnd no longer
// closes a port handle a pending write still names (the device's own, already closed or about to be).
// Left as they are: kill_thread's 12 s wait and panic (nothing in v1.0 ever sets the task it waits for: it returns at
// once) and LineDevice::Recv's ReadFile without an OVERLAPPED on the overlapped port (kernel32 waits on the handle when
// a read pends, and with cfg_com_timeouts' return-at-once timeouts a read never pends).
//
// Footprints: replay_only for everything that touches a COM port, TAPI, the registry, events or tasks, allocates or frees
// -- no line hardware here, so test/world_net_line.cpp checks every one offline against the originals on a fake COM port,
// a fake TAPI (scripted call / answer / reply events) and a fake registry, and a session replay of a line game would be
// the in-game check. The accessors are pure; the device-info initialisers write only their entry.
//
// Also here: the bare `ret`s (dump_blocks, reclaim_all_sendblocks, PreLineBegin, PreLineEnd, TAPI_VERBOSE,
// DirectLine::Shutdown), the constant stubs (tapidev_in_use, DirectLine::CanDestroySafely) and the deleting destructors.
// Left to tools/gen_leftovers.py --lib multi (group B's net_leftover.cpp): line.obj's $E initialisers (and the Xlators'
// atexit destructor helpers $E9..$E19).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_wsock.h"

namespace {
namespace net_line {
using namespace nt;

#define K32(T, slot) NT_IAT(T, slot)
#define TAPI(T, slot) NT_IAT(T, slot)
static __forceinline const char* tapierr(int32_t r) { return ccall<const char*>(A_tapierr, r); }
static __forceinline const char* win32_error() { return ccall<const char*>(F_Win32GetErrorString0); }
static __forceinline SendBlock* block(int32_t i) { return (SendBlock*)(uintptr_t)(S_BLOCKS + (uint32_t)i * sizeof(SendBlock)); }

// the function-local Xlators {key, value, cookie}: built once (guard bit, Xlator::Xlator, atexit of its destructor helper)
static __forceinline void xl_once(uint32_t guard, uint8_t bit, uint32_t xl, uint32_t key, uint32_t dtor) {
    if (!(NT_G8(guard) & bit)) {
        NT_G8(guard) = (uint8_t)(NT_G8(guard) | bit);
        tcall<void*>(F_Xlator_ctor, (void*)(uintptr_t)xl, NT_CP(key));
        ccall<int32_t>(F_atexit, dtor);
    }
}
static __forceinline uint32_t xlate(uint32_t xl) {
    if (NT_GU32(xl + 8) != NT_GU32(S_XLATOR_COOKIE)) tcall<void>(F_Xlator_xlate, (void*)(uintptr_t)xl);
    return NT_GU32(xl + 4);
}
static __forceinline bool xl_built(uint32_t guard, uint8_t bits) { return (NT_G8(guard) & bits) == bits; }

// a harness's marker for "a fix changed what happens here" (nothing in the DLL)
#ifndef VP_FIX_HIT
#define VP_FIX_HIT(what) ((void)0)
#endif

template <typename T> static void fp_pure(Footprint& f, T*, Edx) { f.pure = true; }
template <typename T> static void fp_line_io(Footprint& f, T*, Edx) { f.replay_only = "talks to the COM port or TAPI"; }
template <typename T> static void fp_status_out(Footprint& f, T* self, Edx) {
    f.pure = true;
    f.add(self, sizeof(LineDevice), "the device");
}

// =========================================================================================================================
// LineDevice
// =========================================================================================================================
// Send(p, n): connected only (else logged); the finished blocks freed, the packet copied into a free block (none: dropped)
// and written overlapped (an error other than "pending" logged). FIX: a length outside 0..0xe4 drops the packet before a
// block is taken (the original copied it into the block's 0xe4 bytes unchecked, over the next blocks).
static void __fastcall LineDevice_Send(LineDevice* self, Edx, const void* p, int32_t n) {
    ccall<void>(A_dump_blocks);
    const int32_t st = self->status;
    if (st != 2) {
        NT_LogReport(NT_CP(0x004faaec), ccall<const char*>(A_lds_str, st));
        return;
    }
    ccall<void>(A_free_completed_sendblocks);
    if (VP_FIX && (uint32_t)n > 0xe4u) {
        VP_FIX_HIT("LineDevice::Send");
        return;
    }
    SendBlock* b = ccall<SendBlock*>(A_alloc_sendblock, self->handle);
    if (!b) return;
    crt_copy(b->data, p, (uint32_t)n);
    if (K32(WriteFile_f, I_WriteFile)(self->handle, b->data, (uint32_t)n, (uint32_t*)(uintptr_t)S_WRITTEN, b->ovl)) return;
    if (K32(Dword0_f, I_GetLastError)() == 0x3e5) return;
    NT_LogReport(NT_CP(0x004faad0), win32_error());
}
static void fp_LineDevice_Send(Footprint& f, LineDevice*, Edx, const void*, int32_t) { f.replay_only = "writes the COM port"; }
PORT_FN(0x004a00e0, "LineDevice::Send", LineDevice_Send, fp_LineDevice_Send)

// Recv(buf, &n): connected only (else logged, 0); ReadFile of n bytes (its failure logged); n = what was read; 1 if any
static uint8_t __fastcall LineDevice_Recv(LineDevice* self, Edx, void* buf, int32_t* n) {
    uint32_t got;
    const int32_t st = self->status;
    if (st != 2) {
        NT_LogReport(NT_CP(0x004fab34), ccall<const char*>(A_lds_str, st));
        return 0;
    }
    if (!K32(ReadFile_f, I_ReadFile)(self->handle, buf, (uint32_t)*n, &got, 0)) NT_LogReport(NT_CP(0x004fab1c), win32_error());
    *n = (int32_t)got;
    return (int32_t)got > 0 ? 1 : 0;
}
static void fp_LineDevice_Recv(Footprint& f, LineDevice*, Edx, void*, int32_t*) { f.replay_only = "reads the COM port"; }
PORT_FN(0x004a01a0, "LineDevice::Recv", LineDevice_Recv, fp_LineDevice_Recv)

// DataAvail / DataPending: the bytes waiting in / to go out (ClearCommError), when connected; an error resets the line
static int32_t __fastcall LineDevice_DataAvail(LineDevice* self, Edx) {
    uint32_t stat[4];                                               // errors, then COMSTAT {flags, cbInQue, cbOutQue}
    const void* vt = vtbl_of(self);
    if (vtcall<int32_t>(vt, LD_GET_STATUS, self) != 2) return 0;
    if (K32(ClearCommError_f, I_ClearCommError)(self->handle, &stat[0], &stat[1])) return (int32_t)stat[2];
    NT_LogReport(NT_CP(0x004fab68), ccall<const char*>(F_Win32GetErrorString1, K32(Dword0_f, I_GetLastError)()));
    vtcall<void>(vt, LD_RESET, self);
    return 0;
}
PORT_FN(0x004a0220, "LineDevice::DataAvail", LineDevice_DataAvail, fp_line_io<LineDevice>)

static int32_t __fastcall LineDevice_DataPending(LineDevice* self, Edx) {
    uint32_t stat[4];
    const void* vt = vtbl_of(self);
    if (vtcall<int32_t>(vt, LD_GET_STATUS, self) != 2) return 0;
    if (K32(ClearCommError_f, I_ClearCommError)(self->handle, &stat[0], &stat[1])) return (int32_t)stat[3];
    NT_LogReport(NT_CP(0x004fab98), win32_error());
    vtcall<void>(vt, LD_RESET, self);
    return 0;
}
PORT_FN(0x004a0290, "LineDevice::DataPending", LineDevice_DataPending, fp_line_io<LineDevice>)

static int32_t __fastcall LineDevice_GetStatus(LineDevice* self, Edx) { return self->status; }
PORT_FN(0x004a02f0, "LineDevice::GetStatus", LineDevice_GetStatus, fp_pure<LineDevice>)

// cfg_com_timeouts: reads return at once (ReadIntervalTimeout MAXDWORD, the rest 0)
static uint8_t __fastcall LineDevice_cfg_com_timeouts(LineDevice* self, Edx) {
    uint32_t t[5];
    t[1] = 0;
    t[2] = 0;
    t[3] = 0;
    t[4] = 0;
    t[0] = 0xffffffffu;
    if (K32(HandlePtr_f, I_SetCommTimeouts)(self->handle, t)) return 1;
    NT_LogReport(NT_CP(0x004fabd4), win32_error());
    return 0;
}
PORT_FN(0x004a0300, "LineDevice::cfg_com_timeouts", LineDevice_cfg_com_timeouts, fp_line_io<LineDevice>)

static uint8_t __fastcall LineDevice_Ok(LineDevice* self, Edx) { return self->status != 0 ? 1 : 0; }
PORT_FN(0x004a1fa0, "LineDevice::Ok", LineDevice_Ok, fp_pure<LineDevice>)

// the deleting destructor (LineDevice's destructor inlined: it closes nothing)
static void* __fastcall LineDevice_deleting_dtor(LineDevice* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_LineDevice;
    self->vtbl = (const void*)(uintptr_t)VT_ILineDevice;
    self->handle = 0xffffffffu;
    self->status = 0;
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
static void fp_LineDevice_deleting_dtor(Footprint& f, LineDevice* self, Edx, uint32_t flags) {
    if (flags & 1) f.replay_only = "frees the device";
    else f.add(self, sizeof(LineDevice), "the device");
}
PORT_FN(0x004a1fb0, "LineDevice::scalar deleting destructor",LineDevice_deleting_dtor, fp_LineDevice_deleting_dtor)

// =========================================================================================================================
// TAPILine
// =========================================================================================================================
// TAPILine(device): its device configuration set, the line opened for data-modem calls (this as the callback instance),
// the port handle taken, the port's timeouts set: Ok
static TAPILine* __fastcall TAPILine_ctor(TAPILine* self, Edx, uint32_t devid) {
    self->vtbl = (const void*)(uintptr_t)VT_LineDevice;
    self->handle = 0xffffffffu;
    self->status = 1;
    ccall<void>(A_reclaim_all_sendblocks);
    self->vtbl = (const void*)(uintptr_t)VT_TAPILine;
    self->devid = devid;
    self->ok = 0;
    self->hline = 0;
    self->hcall = 0;
    self->connected = 0;
    if (!tcall<uint8_t>(A_TAPILine_cfg_modem, self)) return self;
    const int32_t r = TAPI(LineOpen_f, I_lineOpen)(NT_GU32(S_HLINEAPP), devid, &self->hline, 0x10004, 0, (uint32_t)(uintptr_t)self,
                                                   4, 0x10, 0);
    NT_G32(S_TAPI_RESULT) = r;
    if (r != 0) {
        NT_LogReport(NT_CP(0x004fac18), devid, tapierr(NT_G32(S_TAPI_RESULT)));
        return self;
    }
    if (!tcall<uint8_t>(A_TAPILine_get_handle, self)) {
        NT_LogReport(NT_CP(0x004fabf0));
        return self;
    }
    if (!tcall<uint8_t>(A_cfg_com_timeouts, self)) return self;
    self->ok = 1;
    return self;
}
static void fp_TAPILine_ctor(Footprint& f, TAPILine*, Edx, uint32_t) { f.replay_only = "opens a TAPI line"; }
PORT_FN(0x004a0350, "TAPILine::TAPILine", TAPILine_ctor, fp_TAPILine_ctor)

static void __fastcall TAPILine_dtor(TAPILine* self, Edx) {
    const uint32_t h = self->handle;
    self->vtbl = (const void*)(uintptr_t)VT_TAPILine;
    if (h != 0xffffffffu) {
        K32(Handle_f, I_CloseHandle)(h);
        self->handle = 0xffffffffu;
    }
    if (self->hcall) {
        TAPI(Tapi3_f, I_lineDrop)(self->hcall, 0, 0);
        TAPI(Tapi1_f, I_lineDeallocateCall)(self->hcall);
        self->hcall = 0;
    }
    if (self->hline) {
        TAPI(Tapi1_f, I_lineClose)(self->hline);
        self->hline = 0;
    }
    self->vtbl = (const void*)(uintptr_t)VT_LineDevice;
    self->ok = 0;
    self->status = 0;
    self->handle = 0xffffffffu;
    self->vtbl = (const void*)(uintptr_t)VT_ILineDevice;
}
PORT_FN(0x004a0420, "TAPILine::~TAPILine", TAPILine_dtor, fp_line_io<TAPILine>)

// get_handle: the line's COM handle (lineGetID "comm/datamodem": a VARSTRING, binary {HANDLE, name}); 0 counts as none.
// FIX: a string offset from the driver past the 0x200-byte VARSTRING gives no handle (the original read wherever it
// pointed).
static uint8_t __fastcall TAPILine_get_handle(TAPILine* self, Edx) {
    uint8_t vs[0x200];
    crt_zero(vs, 0x80);
    const char* cls = NT_CP(NT_GU32(S_DATAMODEM));
    *(uint32_t*)vs = 0x200;
    const int32_t r = TAPI(LineGetID_f, I_lineGetID)(self->hline, 0, 0, 1, vs, cls);
    NT_G32(S_TAPI_RESULT) = r;
    if (r == 0) {
        uint32_t format, offset;
        memcpy(&format, vs + 0xc, 4);
        memcpy(&offset, vs + 0x14, 4);
        if (format != 4) {
            NT_LogReport(NT_CP(0x004fac54), format);
        } else if (VP_FIX && offset > 0x200u - 4u) {
            VP_FIX_HIT("TAPILine::get_handle");
        } else {
            uint32_t h;
            memcpy(&h, vs + offset, 4);
            self->handle = h;
            memcpy(&offset, vs + 0x14, 4);
            NT_TAPI_VERBOSE(NT_CP(0x004fac30), h, (const char*)(vs + offset + 4));
            if (self->handle == 0) self->handle = 0xffffffffu;
        }
    }
    return self->handle + 1 < 1u ? 0 : 1;
}
PORT_FN(0x004a04a0, "TAPILine::get_handle", TAPILine_get_handle, fp_line_io<TAPILine>)

static uint8_t __fastcall TAPILine_cfg_modem(TAPILine* self, Edx) { return ccall<uint8_t>(A_set_from_bytestream, self->devid); }
PORT_FN(0x004a0540, "TAPILine::cfg_modem", TAPILine_cfg_modem, fp_line_io<TAPILine>)

// set_from_bytestream(device): the modem's settings, a fixed 0x6c-byte configuration (every byte written, as the original
// writes it byte by byte on its stack), set, read back and compared (a difference only logged); 0 if the set failed.
// FIX: the configuration read back is compared only when the driver's offset and size lie inside the 0x400-byte VARSTRING
// (the original compared wherever they pointed, for as long as they said).
static uint8_t __cdecl set_from_bytestream_c(uint32_t devid) {
    static const uint8_t k_nonzero[][2] = {
        {0x00, 0x6c}, {0x04, 0x03}, {0x06, 0x01}, {0x0a, 0x08}, {0x0c, 0x60}, {0x10, 0x01}, {0x14, 0x1c}, {0x19, 0xe1},
        {0x1c, 0x15}, {0x1d, 0x20}, {0x22, 0x0a}, {0x24, 0x0a}, {0x26, 0x08}, {0x29, 0x11}, {0x2a, 0x13}, {0x30, 0x06},
        {0x34, 0x30}, {0x38, 0x30}, {0x3c, 0x30}, {0x40, 0x30}, {0x4c, 0x3c}, {0x5c, 0x50}, {0x5d, 0x03},
    };
    struct {
        uint8_t cfg[0x6c];
        uint8_t vs[0x400];                                          // straight after it, as on the original's stack
    } l;
    memset(l.cfg, 0, sizeof l.cfg);
    for (const auto& e : k_nonzero) l.cfg[e[0]] = e[1];
    const char* cls = NT_CP(NT_GU32(S_DATAMODEM));
    int32_t r = TAPI(LineSetDevConfig_f, I_lineSetDevConfig)(devid, l.cfg, 0x6c, cls);
    NT_G32(S_TAPI_RESULT) = r;
    if (r != 0) {
        NT_LogReport(NT_CP(0x004facc4), tapierr(NT_G32(S_TAPI_RESULT)));
        return 0;
    }
    *(uint32_t*)l.vs = 0x400;
    r = TAPI(LineDevConfig_f, I_lineGetDevConfig)(devid, l.vs, cls);
    NT_G32(S_TAPI_RESULT) = r;
    if (r != 0) {
        NT_LogReport(NT_CP(0x004faca8), tapierr(NT_G32(S_TAPI_RESULT)));
        return 1;
    }
    uint32_t size, offset;
    memcpy(&offset, l.vs + 0x14, 4);
    memcpy(&size, l.vs + 0x10, 4);
    if (VP_FIX && (offset > 0x400u || size > 0x400u - offset)) {
        VP_FIX_HIT("set_from_bytestream");
        return 1;
    }
    if (memcmp(l.vs + offset, l.cfg, size) != 0) NT_LogReport(NT_CP(0x004fac7c));
    return 1;
}
static void fp_set_from_bytestream(Footprint& f, uint32_t) { f.replay_only = "sets the modem's configuration (TAPI)"; }
PORT_FN(0x004a0550, "set_from_bytestream", set_from_bytestream_c, fp_set_from_bytestream)

// detect_cd: the port's DCD (MS_RLSD_ON)
static uint8_t __fastcall TAPILine_detect_cd(TAPILine* self, Edx) {
    uint32_t st;
    const uint32_t h = self->handle;
    if (h == 0xffffffffu) return 0;
    if (!K32(HandlePtr_f, I_GetCommModemStatus)(h, &st)) return 0;
    return (st & 0x80) ? 1 : 0;
}
PORT_FN(0x004a0810, "TAPILine::detect_cd", TAPILine_detect_cd, fp_line_io<TAPILine>)

// update_callbps: the call's rate (LINECALLINFO dwRate), while a call is up
static void __fastcall TAPILine_update_callbps(TAPILine* self, Edx) {
    uint8_t ci[0x200];
    if (!self->connected) return;
    crt_zero(ci, 0x80);
    *(uint32_t*)ci = 0x200;
    const int32_t r = TAPI(Tapi2_f, I_lineGetCallInfo)(self->hcall, ci);
    NT_G32(S_TAPI_RESULT) = r;
    if (r == 0) {
        memcpy(&self->rate, ci + 0x1c, 4);
        return;
    }
    NT_LogReport(NT_CP(0x004face0), tapierr(NT_G32(S_TAPI_RESULT)));
    self->rate = 0;
}
PORT_FN(0x004a0840, "TAPILine::update_callbps", TAPILine_update_callbps, fp_line_io<TAPILine>)

// update_callstate(state, hcall, p2, privilege): the line's status from LINECALLSTATE_: idle 1, offering (as owner: the
// call taken, 10; a second one resets first), dialtone 5, dialing 6, ringback 8, busy 9, connected 2 (and its rate),
// proceeding 7, disconnected -> Reset; accepted, the unexpected ones and unknown ones only logged
static void __fastcall TAPILine_update_callstate(TAPILine* self, Edx, uint32_t state, uint32_t hcall, uint32_t, uint32_t priv) {
    self->callstate = state;
    NT_TAPI_VERBOSE(NT_CP(0x004fad04), ccall<const char*>(A_tapicallstate2str, state));
    switch (state) {
    case 0x1:
        self->status = 1;
        return;
    case 0x2:
        if (priv != 4) {
            NT_LogReport(NT_CP(0x004fad54), priv);
            return;
        }
        if (self->connected) {
            NT_LogReport(NT_CP(0x004fad14));
            vcall<void>(self, LD_RESET);
        }
        self->connected = 1;
        self->status = 0xa;
        self->hcall = hcall;
        return;
    case 0x4:
        NT_TAPI_VERBOSE(NT_CP(0x004fad80));
        return;
    case 0x8:
        self->status = 5;
        return;
    case 0x10:
        self->status = 6;
        return;
    case 0x20:
        self->status = 8;
        return;
    case 0x40:
        self->status = 9;
        return;
    case 0x80: case 0x400: case 0x800: case 0x1000: case 0x2000: case 0x8000:
        NT_LogReport(NT_CP(0x004fad98), ccall<const char*>(A_tapicallstate2str, state));
        return;
    case 0x100:
        self->status = 2;
        tcall<void>(A_TAPILine_update_callbps, self);
        return;
    case 0x200:
        self->status = 7;
        return;
    case 0x4000:
        vcall<void>(self, LD_RESET);
        return;
    default:
        NT_LogReport(NT_CP(0x004fadb8), state);
        return;
    }
}
static void fp_TAPILine_update_callstate(Footprint& f, TAPILine*, Edx, uint32_t, uint32_t, uint32_t, uint32_t) {
    f.replay_only = "a TAPI event";
}
PORT_FN(0x004a08c0, "TAPILine::update_callstate", TAPILine_update_callstate, fp_TAPILine_update_callstate)

static void __fastcall TAPILine_line_close(TAPILine* self, Edx) {
    vcall<void>(self, LD_RESET);
    self->hline = 0;
}
PORT_FN(0x004a0aa0, "TAPILine::line_close", TAPILine_line_close, fp_line_io<TAPILine>)

// message(hdevice, msg, p1, p2, p3): TAPI's callback for this line: LINE_CALLSTATE (2), LINE_CLOSE (3), LINE_REPLY (12:
// the request p1 finished with result p2 -- the call's (connected, or reset), the answer's, the drop's (deallocate the
// call), two more only checked; an unknown request logged)
static void __fastcall TAPILine_message(TAPILine* self, Edx, uint32_t hdev, uint32_t msg, uint32_t p1, uint32_t p2, uint32_t p3) {
    NT_TAPI_VERBOSE(NT_CP(0x004fadd4), ccall<const char*>(A_tapimsg2str, msg));
    if (msg == 2) {
        NT_TAPI_VERBOSE(NT_CP(0x004faea8), ccall<const char*>(A_tapicallstate2str, p1));
        tcall<void>(A_TAPILine_update_callstate, self, p1, hdev, p2, p3);
        return;
    }
    if (msg == 3) {
        tcall<void>(A_TAPILine_line_close, self);
        return;
    }
    if (msg != 0xc) return;
    const int32_t id = (int32_t)p1, result = (int32_t)p2;
    if (self->req_call == id) {
        if (result == 0) {
            self->connected = 1;
            return;
        }
        NT_LogReport(NT_CP(0x004fadf0), tapierr(result));
        vcall<void>(self, LD_RESET);
        return;
    }
    if (self->req_answer == id) {
        if (result == 0) return;
        NT_LogReport(NT_CP(0x004fae10), tapierr(result));
        vcall<void>(self, LD_RESET);
        return;
    }
    if (self->req_drop == id) {
        const int32_t r = TAPI(Tapi1_f, I_lineDeallocateCall)(self->hcall);
        if (r == 0) {
            self->hcall = 0;
            return;
        }
        NT_LogReport(NT_CP(0x004fae30), tapierr(r));
        return;
    }
    if (self->req_2c == id) {
        if (result == 0) return;
        NT_LogReport(NT_CP(0x004fae5c), tapierr(result));
        vcall<void>(self, LD_RESET);
        return;
    }
    if (self->req_30 == id) return;
    NT_LogReport(NT_CP(0x004fae78));
}
static void fp_TAPILine_message(Footprint& f, TAPILine*, Edx, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
    f.replay_only = "a TAPI event";
}
PORT_FN(0x004a0ac0, "TAPILine::message", TAPILine_message, fp_TAPILine_message)

static uint8_t __fastcall TAPILine_Ok(TAPILine* self, Edx) {
    if (self->status == 0) return 0;
    if (NT_G8(S_TAPI_OK) == 0) return 0;
    if (self->handle == 0xffffffffu) return 0;
    if (self->ok == 0) return 0;
    return 1;
}
static void fp_TAPILine_Ok(Footprint&, TAPILine*, Edx) {}
PORT_FN(0x004a0c20, "TAPILine::Ok", TAPILine_Ok, fp_TAPILine_Ok)

static void __fastcall TAPILine_Shutdown(TAPILine* self, Edx) { vcall<void>(self, LD_RESET); }
PORT_FN(0x004a0c50, "TAPILine::Shutdown", TAPILine_Shutdown, fp_line_io<TAPILine>)

static uint8_t __fastcall TAPILine_CanDestroySafely(TAPILine* self, Edx) { return self->hcall == 0 ? 1 : 0; }
PORT_FN(0x004a0c60, "TAPILine::CanDestroySafely", TAPILine_CanDestroySafely, fp_pure<TAPILine>)

static int32_t __fastcall TAPILine_BPS(TAPILine* self, Edx) {
    int32_t r = 0;
    if (self->status == 2) r = self->rate;
    return r / 10;
}
PORT_FN(0x004a0c70, "TAPILine::BPS", TAPILine_BPS, fp_pure<TAPILine>)

// Call(number): from idle only (else logged): lineMakeCall, data modem 9600..19200 bps; a request id > 0 -> calling (4)
static void __fastcall TAPILine_Call(TAPILine* self, Edx, const char* number) {
    uint32_t cp[0x1c];
    self->number = number;
    if (self->status != 1) {
        NT_LogReport(NT_CP(0x004faedc));
        return;
    }
    crt_zero(cp, 0x1c);
    cp[0] = 0x70;
    cp[1] = 1;
    cp[2] = 0x2580;
    cp[3] = 0x4b00;
    cp[4] = 0x10;
    const int32_t r = TAPI(LineMakeCall_f, I_lineMakeCall)(self->hline, &self->hcall, number, 0, cp);
    self->req_call = r;
    if (r > 0) {
        self->status = 4;
        return;
    }
    NT_LogReport(NT_CP(0x004faec4), tapierr(r));
    self->hcall = 0;
}
static void fp_TAPILine_Call(Footprint& f, TAPILine*, Edx, const char*) { f.replay_only = "makes a TAPI call"; }
PORT_FN(0x004a0c90, "TAPILine::Call", TAPILine_Call, fp_TAPILine_Call)

// Reset: a call that is up dropped (the request id kept); an idle call deallocated; back to idle (1)
static void __fastcall TAPILine_Reset(TAPILine* self, Edx) {
    if (self->connected) {
        const int32_t r = TAPI(Tapi3_f, I_lineDrop)(self->hcall, 0, 0);
        self->req_drop = r;
        if (r < 0) NT_LogReport(NT_CP(0x004faefc), tapierr(r));
    }
    const uint32_t c = self->hcall;
    if (c && self->callstate == 1) {
        const int32_t r = TAPI(Tapi1_f, I_lineDeallocateCall)(c);
        NT_G32(S_TAPI_RESULT) = r;
        if (r == 0) self->hcall = 0;
        else NT_LogReport(NT_CP(0x004faf14), tapierr(NT_G32(S_TAPI_RESULT)));
    }
    self->connected = 0;
    self->status = 1;
}
PORT_FN(0x004a0d50, "TAPILine::Reset", TAPILine_Reset, fp_line_io<TAPILine>)

// Answer: an offered call (10) answered: a request id > 0 -> answering (11), else logged and reset
static void __fastcall TAPILine_Answer(TAPILine* self, Edx) {
    if (self->status != 0xa) {
        NT_LogReport(NT_CP(0x004faf4c), ccall<const char*>(A_tapicallstate2str, self->callstate));
        return;
    }
    const int32_t r = TAPI(Tapi3_f, I_lineAnswer)(self->hcall, 0, 0);
    self->req_answer = r;
    if (r > 0) {
        self->status = 0xb;
        return;
    }
    NT_LogReport(NT_CP(0x004faf34), tapierr(r));
    vcall<void>(self, LD_RESET);
}
PORT_FN(0x004a0de0, "TAPILine::Answer", TAPILine_Answer, fp_line_io<TAPILine>)

// =========================================================================================================================
// DirectLine
// =========================================================================================================================
static LineDevice* __fastcall DirectLine_ctor(LineDevice* self, Edx, const char* name) {
    self->vtbl = (const void*)(uintptr_t)VT_LineDevice;
    self->handle = 0xffffffffu;
    self->status = 1;
    ccall<void>(A_reclaim_all_sendblocks);
    self->vtbl = (const void*)(uintptr_t)VT_DirectLine;
    const uint32_t h = K32(CreateFileA_f, I_CreateFileA)(name, 0xc0000000u, 0, 0, 3, 0x40000000u, 0);
    self->handle = h;
    if (h == 0xffffffffu) return self;
    if (tcall<uint8_t>(A_DirectLine_cfg_port, self)) return self;
    K32(Handle_f, I_CloseHandle)(self->handle);
    self->handle = 0xffffffffu;
    return self;
}
static void fp_DirectLine_ctor(Footprint& f, LineDevice*, Edx, const char*) { f.replay_only = "opens a COM port"; }
PORT_FN(0x004a0e50, "DirectLine::DirectLine", DirectLine_ctor, fp_DirectLine_ctor)

static uint8_t __fastcall DirectLine_Ok(LineDevice* self, Edx) {
    if (self->status == 0) return 0;
    if (self->handle == 0xffffffffu) return 0;
    return 1;
}
PORT_FN(0x004a0ec0, "DirectLine::Ok", DirectLine_Ok, fp_pure<LineDevice>)

// update_status: while connected or dialled (2, 3): DCD up -> connected; down -> a connected line back to idle
static void __fastcall DirectLine_update_status(LineDevice* self, Edx) {
    uint32_t ms;
    const int32_t st = self->status;
    if (st != 3 && st != 2) return;
    if (!tcall<uint8_t>(A_DirectLine_check_lines, self, &ms)) return;
    if (ms & 0x80) {
        self->status = 2;
        return;
    }
    if (self->status == 2) self->status = 1;
}
PORT_FN(0x004a0ee0, "DirectLine::update_status", DirectLine_update_status, fp_line_io<LineDevice>)

static int32_t __fastcall DirectLine_GetStatus(LineDevice* self, Edx) {
    tcall<void>(A_DirectLine_update_status, self);
    return tcall<int32_t>(A_LineDevice_GetStatus, self);
}
PORT_FN(0x004a0f30, "DirectLine::GetStatus", DirectLine_GetStatus, fp_line_io<LineDevice>)

static uint8_t __fastcall DirectLine_check_lines(LineDevice* self, Edx, uint32_t* ms) {
    const uint32_t h = self->handle;
    if (h == 0xffffffffu) return 0;
    return K32(HandlePtr_f, I_GetCommModemStatus)(h, ms) ? 1 : 0;
}
static void fp_DirectLine_check_lines(Footprint& f, LineDevice*, Edx, uint32_t*) { f.replay_only = "reads the modem lines"; }
PORT_FN(0x004a0f50, "DirectLine::check_lines", DirectLine_check_lines, fp_DirectLine_check_lines)

static void __fastcall DirectLine_dtor(LineDevice* self, Edx) {
    const uint32_t h = self->handle;
    self->vtbl = (const void*)(uintptr_t)VT_DirectLine;
    if (h != 0xffffffffu) {
        K32(Handle_f, I_CloseHandle)(h);
        self->handle = 0xffffffffu;
    }
    self->handle = 0xffffffffu;
    self->status = 0;
    self->vtbl = (const void*)(uintptr_t)VT_LineDevice;
    self->vtbl = (const void*)(uintptr_t)VT_ILineDevice;
}
PORT_FN(0x004a0fa0, "DirectLine::~DirectLine", DirectLine_dtor, fp_line_io<LineDevice>)

// port_is_capable: an RS-232 port (COMMPROP dwProvSubType PST_RS232; the rest of the 0x200-byte buffer as the stack has it)
static uint8_t __fastcall DirectLine_port_is_capable(LineDevice* self, Edx) {
    uint8_t cp[0x200];
    const uint16_t len = 0x200;
    const uint32_t spec = 0xe73cf52eu;
    memcpy(cp, &len, 2);
    memcpy(cp + 0x34, &spec, 4);
    if (!K32(HandlePtr_f, I_GetCommProperties)(self->handle, cp)) {
        NT_LogReport(NT_CP(0x004fafa4), win32_error());
        return 0;
    }
    int32_t sub;
    memcpy(&sub, cp + 0x18, 4);
    if (sub == 1) return 1;
    NT_LogReport(NT_CP(0x004faf78), ccall<const char*>(A_pst_type, sub));
    return 0;
}
PORT_FN(0x004a0fe0, "DirectLine::port_is_capable", DirectLine_port_is_capable, fp_line_io<LineDevice>)

// cfg_port: an RS-232 port set to 19200 8N1 with RTS/CTS (init_dcb), then the timeouts
static uint8_t __fastcall DirectLine_cfg_port(LineDevice* self, Edx) {
    uint8_t dcb[0x1c];
    if (!tcall<uint8_t>(A_DirectLine_port_is_capable, self)) return 0;
    if (!K32(HandlePtr_f, I_GetCommState)(self->handle, dcb)) {
        NT_LogReport(NT_CP(0x004faff4), win32_error());
        return 0;
    }
    ccall<void>(A_init_dcb, (void*)dcb, (uint32_t)0x4b00);
    if (!K32(HandlePtr_f, I_SetCommState)(self->handle, dcb)) {
        NT_LogReport(NT_CP(0x004fafd8), win32_error());
        return 0;
    }
    return tcall<uint8_t>(A_cfg_com_timeouts, self);
}
PORT_FN(0x004a1060, "DirectLine::cfg_port", DirectLine_cfg_port, fp_line_io<LineDevice>)

static int32_t __fastcall DirectLine_BPS(LineDevice*, Edx) { return 0x780; }
PORT_FN(0x004a10f0, "DirectLine::BPS", DirectLine_BPS, fp_pure<LineDevice>)

static void __fastcall DirectLine_Call(LineDevice* self, Edx, const char*) { self->status = 3; }
static void fp_DirectLine_Call(Footprint& f, LineDevice* self, Edx, const char*) {
    f.pure = true;
    f.add(self, sizeof(LineDevice), "the device");
}
PORT_FN(0x004a1100, "DirectLine::Call", DirectLine_Call, fp_DirectLine_Call)
static void __fastcall DirectLine_Answer(LineDevice* self, Edx) { self->status = 3; }
PORT_FN(0x004a1110, "DirectLine::Answer", DirectLine_Answer, fp_status_out<LineDevice>)
static void __fastcall DirectLine_Reset(LineDevice* self, Edx) { self->status = 1; }
PORT_FN(0x004a1120, "DirectLine::Reset", DirectLine_Reset, fp_status_out<LineDevice>)

static void __fastcall DirectLine_Send(LineDevice* self, Edx, const void* p, int32_t n) {
    tcall<void>(A_DirectLine_update_status, self);
    tcall<void>(A_LineDevice_Send, self, p, n);
}
static void fp_DirectLine_Send(Footprint& f, LineDevice*, Edx, const void*, int32_t) { f.replay_only = "writes the COM port"; }
PORT_FN(0x004a2040, "DirectLine::Send", DirectLine_Send, fp_DirectLine_Send)

// =========================================================================================================================
// the line library
// =========================================================================================================================
// LineDeviceInfo::GetError: the last TAPI result's text, translated (LINEERR_NODEVICE or anything else)
static const char* __fastcall LineDeviceInfo_GetError(void*, Edx) {
    xl_once(0x0057c164, 0x01, 0x0057d198, 0x004fb010, 0x004a11e0);
    xl_once(0x0057c164, 0x02, 0x0057c158, 0x004fb030, 0x004a11d0);
    if (NT_GU32(S_TAPI_RESULT) == 0x8000004bu) return NT_CP(xlate(0x0057d198));
    return NT_CP(xlate(0x0057c158));
}
static void fp_LineDeviceInfo_GetError(Footprint& f, void*, Edx) {
    if (!xl_built(0x0057c164, 3)) {
        f.replay_only = "the first call builds its Xlators (atexit)";
        return;
    }
    f.add((void*)(uintptr_t)0x0057d198, 12, "a function's static Xlator");
    f.add((void*)(uintptr_t)0x0057c158, 12, "a function's static Xlator");
}
PORT_FN(0x004a1130, "LineDeviceInfo::GetError", LineDeviceInfo_GetError, fp_LineDeviceInfo_GetError)

// init_dcb(dcb, baud): zeroed, then 8N1, binary, CTS flow out, DTR on, RTS handshake, XON/XOFF limits 10, DC1 / DC3
static void __cdecl init_dcb_c(void* dcbp, uint32_t baud) {
    volatile uint8_t* d = (volatile uint8_t*)dcbp;
    crt_zero(dcbp, 7);
    *(volatile uint32_t*)(d + 0) = 0x1c;
    *(volatile uint32_t*)(d + 4) = baud;
    volatile uint32_t* fl = (volatile uint32_t*)(d + 8);
    uint32_t f = *fl | 1;
    *fl = f;
    f &= ~2u;
    *fl = f;
    f |= 4;
    *fl = f;
    f &= ~8u;
    *fl = f;
    f = (f & ~0x20u) | 0x10;
    *fl = f;
    f &= ~0x40u;
    *fl = f;
    f &= ~0x80u;
    *fl = f;
    f &= ~0x100u;
    *fl = f;
    f &= ~0x200u;
    *fl = f;
    f &= ~0x400u;
    *fl = f;
    f &= ~0x800u;
    *fl = f;
    f = (f & ~0x1000u) | 0x2000;
    *(volatile uint16_t*)(d + 0xc) = 0;
    *fl = f;
    f &= ~0x4000u;
    d[0x12] = 8;
    *fl = f;
    d[0x13] = 0;
    f &= 0x7fff;
    d[0x14] = 0;
    *fl = f;
    d[0x17] = 0;
    d[0x18] = 0;
    *(volatile uint16_t*)(d + 0xe) = 10;
    d[0x19] = 0;
    *(volatile uint16_t*)(d + 0x10) = 10;
    d[0x15] = 0x11;
    d[0x16] = 0x13;
}
static void fp_init_dcb(Footprint& f, void* dcb, uint32_t) {
    f.pure = true;
    f.add(dcb, 0x1c, "the DCB");
}
PORT_FN(0x004a11f0, "init_dcb", init_dcb_c, fp_init_dcb)

// free_completed_sendblocks: each block in use polled (WaitForSingleObject, 0 ms): done, abandoned (logged), failed
// (logged) or anything else (a panic) frees it; still pending keeps it. DBGLineBlocksFree counts the free ones.
static void __cdecl free_completed_sendblocks_c() {
    NT_G32(S_BLOCKS_FREE) = 0x10;
    const Wait_f wait = K32(Wait_f, I_WaitForSingleObject);
    for (uint32_t a = S_BLOCKS; a < S_BLOCKS_END; a += sizeof(SendBlock)) {
        SendBlock* b = (SendBlock*)(uintptr_t)a;
        if (*(volatile uint32_t*)&b->handle == 0xffffffffu) continue;
        const uint32_t ev = b->event;
        NT_G32(S_BLOCKS_FREE) = NT_G32(S_BLOCKS_FREE) - 1;
        const uint32_t r = wait(ev, 0);
        if (r == 0x102) continue;
        if (r == 0x80) NT_LogReport(NT_CP(0x004fb044));
        else if (r == 0xffffffffu) NT_LogReport(NT_CP(0x004fb070), win32_error());
        else if (r != 0) NT_LogPanic(NT_CP(0x004fb098), r);
        b->handle = 0xffffffffu;
    }
}
static void fp_free_completed_sendblocks(Footprint& f) { f.replay_only = "polls the overlapped writes"; }
PORT_FN(0x004a12c0, "free_completed_sendblocks", free_completed_sendblocks_c, fp_free_completed_sendblocks)

static SendBlock* __cdecl alloc_sendblock_c(uint32_t handle) {
    int32_t i = 0;
    uint32_t a = S_BLOCKS;
    for (;;) {
        if (*(volatile uint32_t*)(uintptr_t)a == 0xffffffffu) break;
        a += sizeof(SendBlock);
        i++;
        if (a >= S_BLOCKS_END) return 0;
    }
    SendBlock* b = block(i);
    b->handle = handle;
    volatile uint32_t* o = b->ovl;
    o[3] = 0;
    o[2] = 0;
    o[1] = 0;
    o[0] = 0;
    return b;
}
static void fp_alloc_sendblock(Footprint& f, uint32_t) { f.add((void*)(uintptr_t)S_BLOCKS, S_BLOCKS_END - S_BLOCKS, "the send blocks"); }
PORT_FN(0x004a1360, "alloc_sendblock", alloc_sendblock_c, fp_alloc_sendblock)

// lds_str(status): the status's name (12 entries on the stack). FIX: a status outside 0..11 gives entry 0 ("LDS_BOGUS");
// the original read past its table.
static const char* __cdecl lds_str_c(int32_t st) {
    volatile uint32_t t[12];
    t[0] = 0x004fb0c0;
    t[1] = 0x004fb0cc;
    t[2] = 0x004fb0dc;
    t[3] = 0x004fb0ec;
    t[4] = 0x004fb0f8;
    t[5] = 0x004fb108;
    t[6] = 0x004fb118;
    t[7] = 0x004fb124;
    t[8] = 0x004fb134;
    t[9] = 0x004fb144;
    t[10] = 0x004fb150;
    t[11] = 0x004fb160;
    if (VP_FIX && (uint32_t)st >= 12u) {
        VP_FIX_HIT("lds_str");
        st = 0;
    }
    return NT_CP(t[st]);
}
static void fp_lds_str(Footprint&, int32_t) {}
PORT_FN(0x004a13b0, "lds_str", lds_str_c, fp_lds_str)

// LineBegin: the lobby's thread killed, the 16 send blocks freed with a new event each (a failed event: 0), TAPI started
// (tapi_cb as its callback) and its API version agreed for every device (the first failure logged: TAPI off). TAPI's state.
static uint8_t __cdecl LineBegin_c() {
    ccall<void>(A_kill_thread);
    const CreateEventA_f create = K32(CreateEventA_f, I_CreateEventA);
    for (uint32_t a = S_BLOCKS; a < S_BLOCKS + 16 * sizeof(SendBlock); a += sizeof(SendBlock)) {
        SendBlock* b = (SendBlock*)(uintptr_t)a;
        b->handle = 0xffffffffu;
        const uint32_t ev = create(0, 1, 0, 0);
        b->event = ev;
        if (!ev) return 0;
    }
    const char* app = ccall<const char*>(F_Win32AppName);
    const uint32_t inst = ccall<uint32_t>(F_Win32GetAppInstance);
    int32_t r = TAPI(LineInitialize_f, I_lineInitialize)((uint32_t*)(uintptr_t)S_HLINEAPP, inst, (uint32_t)A_tapi_cb, app,
                                                         (uint32_t*)(uintptr_t)S_NDEVS);
    NT_G32(S_TAPI_RESULT) = r;
    if (r != 0) {
        NT_G32(S_TAPI_RESULT) = r;
        NT_LogReport(NT_CP(0x004fb194), tapierr(r));
        return NT_G8(S_TAPI_OK);
    }
    NT_G8(S_TAPI_OK) = 1;
    for (int32_t i = 0; i < NT_G32(S_NDEVS); i++) {
        uint8_t ext[16];                                            // LINEEXTENSIONID
        r = TAPI(LineNegotiate_f, I_lineNegotiateAPIVersion)(NT_GU32(S_HLINEAPP), (uint32_t)i, 0x10004, 0x10004,
                                                             (uint32_t*)(uintptr_t)S_APIVER, ext);
        NT_G32(S_TAPI_RESULT) = r;
        if (r != 0) {
            NT_G32(S_TAPI_RESULT) = r;
            NT_LogReport(NT_CP(0x004fb170), tapierr(r));
            NT_G8(S_TAPI_OK) = 0;
            break;
        }
    }
    return NT_G8(S_TAPI_OK);
}
static void fp_LineBegin(Footprint& f) { f.replay_only = "starts TAPI"; }
PORT_FN(0x004a1420, "LineBegin", LineBegin_c, fp_LineBegin)

// tapi_cb(hdevice, msg, instance, p1, p2, p3): a line's message to its TAPILine (the instance); with none, logged
static void __stdcall tapi_cb_c(uint32_t hdev, uint32_t msg, uint32_t inst, uint32_t p1, uint32_t p2, uint32_t p3) {
    if (inst) {
        tcall<void>(A_TAPILine_message, (void*)(uintptr_t)inst, hdev, msg, p1, p2, p3);
        return;
    }
    NT_LogReport(NT_CP(0x004fb1b4), ccall<const char*>(A_tapimsg2str, msg));
}
static void fp_tapi_cb(Footprint& f, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { f.replay_only = "a TAPI event"; }
PORT_FN(0x004a1530, "tapi_cb", tapi_cb_c, fp_tapi_cb)

// kill_thread: a task of another thread waited for (100 ms steps, the window kept alive; 12 s -> a panic) and destroyed.
// (Not fixed: S_THREAD is read only here and in v1.0 nothing ever sets it, so this returns at once.)
static void __cdecl kill_thread_c() {
    if (NT_G32(S_THREAD) == 0) return;
    if (ccall<int32_t>(F_TaskGetID) == NT_G32(S_THREAD)) return;
    int32_t waited = 0;
    for (;;) {
        if (ccall<uint8_t>(F_TaskIsDead, NT_G32(S_THREAD))) break;
        waited += 100;
        ccall<void>(F_TaskSleep, (int32_t)100);
        ccall<void>(F_Win32Idle);
        if (waited >= 12000) break;
    }
    if (waited >= 12000) NT_LogPanic(NT_CP(0x004fb1d4));
    ccall<void>(F_TaskDestroy, NT_G32(S_THREAD));
    NT_G32(S_THREAD) = 0;
}
static void fp_kill_thread(Footprint& f) { f.replay_only = "waits for and destroys a task"; }
PORT_FN(0x004a1580, "kill_thread", kill_thread_c, fp_kill_thread)

// LineEnd: the finished writes freed, TAPI shut down; a block still writing is logged and its port closed; the events
// closed. FIX: the port isn't closed. The block only names the port handle of the device that wrote it, which the device
// closes itself (DirectLine / TAPILine's destructor, which runs before LineEnd): the original closed that handle value a
// second time -- by then possibly another handle of the game's.
static void __cdecl LineEnd_c() {
    ccall<void>(A_free_completed_sendblocks);
    TAPI(Tapi1_f, I_lineShutdown)(NT_GU32(S_HLINEAPP));
    const Handle_f close = K32(Handle_f, I_CloseHandle);
    NT_GU32(S_HLINEAPP) = 0;
    NT_G8(S_TAPI_OK) = 0;
    for (uint32_t a = S_BLOCKS; a < S_BLOCKS_END; a += sizeof(SendBlock)) {
        SendBlock* b = (SendBlock*)(uintptr_t)a;
        if (*(volatile uint32_t*)&b->handle != 0xffffffffu) {
            NT_LogReport(NT_CP(0x004fb208));
            if (VP_FIX) VP_FIX_HIT("LineEnd");
            else close(b->handle);
            b->handle = 0xffffffffu;
        }
        const uint32_t ev = b->event;
        if (ev) {
            close(ev);
            b->event = 0;
        }
    }
}
static void fp_LineEnd(Footprint& f) { f.replay_only = "shuts TAPI down"; }
PORT_FN(0x004a1600, "LineEnd", LineEnd_c, fp_LineEnd)

static int32_t __cdecl LineEnumerateDevices_c(LineDeviceInfo* info, int32_t max, int32_t flags) {
    return ccall<int32_t>(A_enum_devices, info, max, flags);
}
static void fp_LineEnumerateDevices(Footprint& f, LineDeviceInfo*, int32_t, int32_t) { f.replay_only = "lists TAPI and the COM ports"; }
PORT_FN(0x004a1690, "LineEnumerateDevices", LineEnumerateDevices_c, fp_LineEnumerateDevices)

// enum_devices(info, max, flags): bit 0 the TAPI modems, bit 1 the COM ports (a table of two on the stack, walked bit by
// bit). Flags 2 alone also runs the TAPI pass first (to mark the modems' ports) and then zeroes the count. FIX: only the
// two lists are walked (the original called whatever the stack held past its table for a flag bit above 1).
static int32_t __cdecl enum_devices_c(LineDeviceInfo* info, int32_t max, int32_t flags) {
    typedef void(__cdecl * Enum_t)(LineDeviceInfo*, int32_t, int32_t*);
    struct {
        int32_t count;
        uint32_t fn[2];
    } l;
    l.fn[0] = A_enumerate_tapi_devices;
    l.fn[1] = A_enumerate_comport_devices;
    l.count = 0;
    if ((flags & 3) == 2) {
        ccall<void>(A_enumerate_tapi_devices, info, max, &l.count);
        l.count = 0;
    }
    const volatile uint32_t* fn = l.fn;
    for (int32_t f = flags; f != 0; f >>= 1, fn++) {
        if (VP_FIX && fn == l.fn + 2) {
            VP_FIX_HIT("enum_devices");
            break;
        }
        if (f & 1) ((Enum_t)(uintptr_t)*fn)(info, max, &l.count);
    }
    return l.count;
}
PORT_FN(0x004a16b0, "enum_devices", enum_devices_c, fp_LineEnumerateDevices)

// enumerate_tapi_devices(info, max, &count): every TAPI device that is a voice-bearer data modem, with the COM port it's
// attached to marked as taken (S_PORT_TAPI, ports 1..4); the others logged. FIX: COM4's flag (byte 4) is cleared with the
// others; the original cleared only bytes 0..3, so once a modem on COM4 had been listed COM4 was never offered again.
static void __cdecl enumerate_tapi_devices_c(LineDeviceInfo* info, int32_t max, int32_t* count) {
    uint8_t caps[0x400];
    NT_GU32(S_PORT_TAPI) = 0;
    if (VP_FIX) {
        if (NT_G8(S_PORT_TAPI + 4)) VP_FIX_HIT("enumerate_tapi_devices");
        NT_G8(S_PORT_TAPI + 4) = 0;
    }
    if (NT_GU32(S_NDEVS) == 0) return;
    uint32_t i = 0;
    do {
        if (*count >= max) return;
        *(uint32_t*)caps = 0x400;
        const int32_t r = TAPI(LineGetDevCaps_f, I_lineGetDevCaps)(NT_GU32(S_HLINEAPP), i, NT_GU32(S_APIVER), 0, caps);
        NT_G32(S_TAPI_RESULT) = r;
        if (r != 0) {
            NT_LogReport(NT_CP(0x004fb280), tapierr(NT_G32(S_TAPI_RESULT)));
        } else {
            uint32_t off;
            memcpy(&off, caps + 0x24, 4);
            const char* name = (const char*)(caps + off);
            if (!name) name = NT_CP(0x004fb22c);
            uint32_t bearer, media;
            memcpy(&bearer, caps + 0x34, 4);
            memcpy(&media, caps + 0x3c, 4);
            if (!(bearer & 1)) {
                NT_LogReport(NT_CP(0x004fb25c), name);
            } else if (!(media & 0x10)) {
                NT_LogReport(NT_CP(0x004fb234), name);
            } else {
                const int32_t port = ccall<int32_t>(A_get_tapiline_port, (void*)caps);
                if (port != 0 && port <= 4) NT_G8(S_PORT_TAPI + (uint32_t)port) = 1;
                const uint8_t in_use = ccall<uint8_t>(A_tapidev_in_use, (int32_t)i);
                memcpy(&off, caps + 0x24, 4);
                uint32_t flag;
                memcpy(&flag, caps + 0xec, 4);
                ccall<void>(A_init_devinfo_as_tapiline, &info[*count], (const char*)(caps + off), (int32_t)i,
                            (uint32_t)((flag & 8) >> 3), (uint32_t)in_use);
                ++*count;
            }
        }
        i++;
    } while (i < NT_GU32(S_NDEVS));
}
static void fp_enumerate_devices(Footprint& f, LineDeviceInfo*, int32_t, int32_t*) { f.replay_only = "lists TAPI or the COM ports"; }
PORT_FN(0x004a1730, "enumerate_tapi_devices", enumerate_tapi_devices_c, fp_enumerate_devices)

// get_tapiline_port(caps): the COM port a modem is attached to (1..9), from its driver key's AttachedTo ("COMn"), via the
// device-specific part of its LINEDEVCAPS; 0 if none. FIX: the key is closed (the original left it open, one handle per
// modem per listing), and the driver's offsets must keep the key's name, terminated, inside the 0x400-byte LINEDEVCAPS
// (else 0: the original read wherever they pointed).
static int32_t __cdecl get_tapiline_port_c(const uint8_t* caps) {
    struct {
        uint32_t type, cb, hkey;
        char data[0x40];
    } l;
    uint32_t size, at;
    memcpy(&size, caps + 0xe4, 4);
    if (size == 0) return 0;
    memcpy(&at, caps + 0xe8, 4);
    if (VP_FIX && at > 0x400u - 8u) {
        VP_FIX_HIT("get_tapiline_port");
        return 0;
    }
    const uint8_t* spec = caps + at;
    uint32_t first, rel;
    memcpy(&first, spec, 4);
    if (first == 0) return 0;
    memcpy(&rel, spec + 4, 4);
    if (VP_FIX && (rel >= 0x400u - at || !memchr(spec + rel, 0, 0x400u - at - rel))) {
        VP_FIX_HIT("get_tapiline_port");
        return 0;
    }
    const char* key = (const char*)(spec + rel);
    if (NT_IAT(RegOpenKeyExA_f, I_RegOpenKeyExA)(0x80000002u, key, 0, 0x20019, &l.hkey) != 0) {
        NT_LogReport(NT_CP(0x004fb2ec), key, win32_error());
        return 0;
    }
    l.cb = 0x40;
    if (NT_IAT(RegQueryValueExA_f, I_RegQueryValueExA)(l.hkey, NT_CP(0x004fb29c), 0, &l.type, l.data, &l.cb) != 0) {
        NT_LogReport(NT_CP(0x004fb2d0), win32_error());
        if (VP_FIX) NT_IAT(RegCloseKey_f, I_RegCloseKey)(l.hkey);
        return 0;
    }
    if (VP_FIX) NT_IAT(RegCloseKey_f, I_RegCloseKey)(l.hkey);
    if (l.type != 1) {
        NT_LogReport(NT_CP(0x004fb2a8), l.type);
        return 0;
    }
    const int32_t n = (int32_t)(int8_t)l.data[3] - 0x30;
    if (n < 1 || n > 9) return 0;
    return n;
}
static void fp_get_tapiline_port(Footprint& f, const uint8_t*) { f.replay_only = "reads the registry"; }
PORT_FN(0x004a1870, "get_tapiline_port", get_tapiline_port_c, fp_get_tapiline_port)

static void __cdecl init_devinfo_as_tapiline_c(LineDeviceInfo* info, const char* name, int32_t id, uint32_t flag0, uint32_t flag1) {
    crt_zero(info, 0x14);
    info->flag0 = (uint8_t)flag0;
    info->flag1 = (uint8_t)flag1;
    ccall<char*>(F_strncpy, (char*)info->name, name, (uint32_t)0x40);
    info->name[0x3f] = 0;
    info->create = A_CreateTAPILine;
    info->id = id;
}
static void fp_init_devinfo_as_tapiline(Footprint& f, LineDeviceInfo* info, const char*, int32_t, uint32_t, uint32_t) {
    f.add(info, sizeof(LineDeviceInfo), "the entry");
}
PORT_FN(0x004a1960, "init_devinfo_as_tapiline", init_devinfo_as_tapiline_c, fp_init_devinfo_as_tapiline)

// enumerate_comport_devices(info, max, &count): COM1..4, unless a modem has it: one that opens (closed again) or is in use
// (access denied, flagged); another error than "not found" logged
static void __cdecl enumerate_comport_devices_c(LineDeviceInfo* info, int32_t max, int32_t* count) {
    char name[8];
    memcpy(name, NT_CP(0x004fb308), 4);                              // "COMX"
    name[4] = NT_CP(0x004fb308)[4];
    for (int32_t n = 1; n <= 4; n++) {
        if (*count >= max) return;
        if (NT_G8(S_PORT_TAPI + (uint32_t)n) != 0) continue;
        name[3] = (char)(n + 0x30);
        const uint32_t h = K32(CreateFileA_f, I_CreateFileA)(name, 0xc0000000u, 0, 0, 3, 0x40000000u, 0);
        if (h != 0xffffffffu) {
            K32(Handle_f, I_CloseHandle)(h);
            ccall<void>(A_init_devinfo_as_comport, &info[*count], (const char*)name, n, (uint32_t)0);
            ++*count;
            continue;
        }
        const uint32_t e = K32(Dword0_f, I_GetLastError)();
        if (e == 5) {
            ccall<void>(A_init_devinfo_as_comport, &info[*count], (const char*)name, n, (uint32_t)1);
            ++*count;
        } else if (e != 2) {
            NT_LogReport(NT_CP(0x004fb310), (const char*)name, ccall<const char*>(F_Win32GetErrorString1, e));
        }
    }
}
PORT_FN(0x004a19b0, "enumerate_comport_devices", enumerate_comport_devices_c, fp_enumerate_devices)

static void __cdecl init_devinfo_as_comport_c(LineDeviceInfo* info, const char* name, int32_t port, uint32_t flag1) {
    crt_zero(info, 0x14);
    info->flag0 = 0;
    info->flag1 = (uint8_t)flag1;
    ccall<char*>(F_strncpy, (char*)info->name, name, (uint32_t)0x40);
    info->name[0x3f] = 0;
    info->create = A_CreateDirectLine;
    info->id = port;
}
static void fp_init_devinfo_as_comport(Footprint& f, LineDeviceInfo* info, const char*, int32_t, uint32_t) {
    f.add(info, sizeof(LineDeviceInfo), "the entry");
}
PORT_FN(0x004a1aa0, "init_devinfo_as_comport", init_devinfo_as_comport_c, fp_init_devinfo_as_comport)

// the devices' factories: construct, and delete again (the vtable read before Ok) unless Ok
static void* __cdecl CreateDirectLine_c(const LineDeviceInfo* info) {
    char name[8];
    memcpy(name, NT_CP(0x004fb324), 4);                              // "COMX"
    name[4] = NT_CP(0x004fb324)[4];
    name[3] = (char)(uint8_t)((uint32_t)info->id + 0x30);
    void* p = ccall<void*>(F_MemAlloc, (int32_t)0xc);
    void* d = 0;
    if (p) d = tcall<void*>(A_DirectLine_ctor, p, (const char*)name);
    if (!d) return 0;
    const void* vt = vtbl_of(d);
    if (vtcall<uint8_t>(vt, LD_OK, d)) return d;
    vtcall<void*>(vt, LD_DTOR, d, 1u);
    return 0;
}
static void fp_create_line(Footprint& f, const LineDeviceInfo*) { f.replay_only = "allocates, opens the device"; }
PORT_FN(0x004a1af0, "CreateDirectLine", CreateDirectLine_c, fp_create_line)

static void* __cdecl CreateTAPILine_c(const LineDeviceInfo* info) {
    void* p = ccall<void*>(F_MemAlloc, (int32_t)0x40);
    void* d = 0;
    if (p) d = tcall<void*>(A_TAPILine_ctor, p, (uint32_t)info->id);
    if (!d) return 0;
    const void* vt = vtbl_of(d);
    if (vtcall<uint8_t>(vt, LD_OK, d)) return d;
    vtcall<void*>(vt, LD_DTOR, d, 1u);
    return 0;
}
PORT_FN(0x004a1b60, "CreateTAPILine", CreateTAPILine_c, fp_create_line)

static int32_t __cdecl DBGLineBlocksFree_c() { return NT_G32(S_BLOCKS_FREE); }
static void fp_DBGLineBlocksFree(Footprint&) {}
PORT_FN(0x004a1bb0, "DBGLineBlocksFree", DBGLineBlocksFree_c, fp_DBGLineBlocksFree)

// LineStatusText(status): the status's translated text (twelve entries: 0 a constant, 8 and 10 the same Xlator). FIX: a
// status outside 0..11 gives entry 0 ("*** invalid ***"); the original read past its table.
static const struct { uint32_t guard; uint8_t bit; uint32_t xl, key, dtor; } k_status_xl[] = {
    {0x0057be98, 0x01, 0x0057beb8, 0x004fb344, 0x004a1f90}, {0x0057be98, 0x02, 0x0057c1b8, 0x004fb35c, 0x004a1f80},
    {0x0057be98, 0x04, 0x0057d1b8, 0x004fb374, 0x004a1f70}, {0x0057be98, 0x08, 0x0057c1a8, 0x004fb388, 0x004a1f60},
    {0x0057be98, 0x10, 0x0057c188, 0x004fb3a8, 0x004a1f50}, {0x0057be98, 0x20, 0x0057c198, 0x004fb3bc, 0x004a1f40},
    {0x0057be98, 0x40, 0x0057d188, 0x004fb3d8, 0x004a1f30}, {0x0057be98, 0x80, 0x0057c178, 0x004fb3f4, 0x004a1f20},
    {0x0057bea0, 0x01, 0x0057bea8, 0x004fb408, 0x004a1f10}, {0x0057bea0, 0x02, 0x0057d1a8, 0x004fb420, 0x004a1f00},
};
static const uint32_t k_status_entry[11] = {   // entries 1..11, the Xlator each takes its text from
    0x0057beb8, 0x0057c1b8, 0x0057d1b8, 0x0057c1a8, 0x0057c198, 0x0057c188, 0x0057d188, 0x0057c178, 0x0057d1a8,
    0x0057c178, 0x0057bea8,
};
static const char* __cdecl LineStatusText_c(int32_t st) {
    volatile uint32_t t[12];
    for (const auto& x : k_status_xl) xl_once(x.guard, x.bit, x.xl, x.key, x.dtor);
    t[0] = 0x004fb438;                                              // "*** invalid ***"
    for (int32_t i = 0; i < 11; i++) t[i + 1] = xlate(k_status_entry[i]);
    if (VP_FIX && (uint32_t)st >= 12u) {
        VP_FIX_HIT("LineStatusText");
        st = 0;
    }
    return NT_CP(t[st]);
}
static void fp_LineStatusText(Footprint& f, int32_t) {
    if (!xl_built(0x0057be98, 0xff) || !xl_built(0x0057bea0, 3)) {
        f.replay_only = "the first call builds its Xlators (atexit)";
        return;
    }
    for (const auto& x : k_status_xl) f.add((void*)(uintptr_t)x.xl, 12, "a function's static Xlator");
}
PORT_FN(0x004a1bc0, "LineStatusText", LineStatusText_c, fp_LineStatusText)

// =========================================================================================================================
// the stubs and the deleting destructors
// =========================================================================================================================
static void fp_ret_void(Footprint& f) { f.pure = true; }
static void __cdecl dump_blocks_c() {}
PORT_FN(0x004a0190, "dump_blocks", dump_blocks_c, fp_ret_void)
static void __cdecl reclaim_all_sendblocks_c() {}
PORT_FN(0x004a12b0, "reclaim_all_sendblocks", reclaim_all_sendblocks_c, fp_ret_void)
static void __cdecl PreLineBegin_c() {}
PORT_FN(0x004a1670, "PreLineBegin", PreLineBegin_c, fp_ret_void)
static void __cdecl PreLineEnd_c() {}
PORT_FN(0x004a1680, "PreLineEnd", PreLineEnd_c, fp_ret_void)
static void __cdecl TAPI_VERBOSE_c(const char*) {}               // (variadic: the caller pops what it pushed)
static void fp_verbose(Footprint& f, const char*) { f.pure = true; }
PORT_FN(0x004a2030, "TAPI_VERBOSE", TAPI_VERBOSE_c, fp_verbose)
static uint8_t __cdecl tapidev_in_use_c(int32_t) { return 0; }
static void fp_tapidev_in_use(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x004a1860, "tapidev_in_use", tapidev_in_use_c, fp_tapidev_in_use)

static void __fastcall DirectLine_Shutdown(LineDevice*, Edx) {}
PORT_FN(0x004a0f80, "DirectLine::Shutdown", DirectLine_Shutdown, fp_pure<LineDevice>)
static uint8_t __fastcall DirectLine_CanDestroySafely(LineDevice*, Edx) { return 1; }
PORT_FN(0x004a0f90, "DirectLine::CanDestroySafely", DirectLine_CanDestroySafely, fp_pure<LineDevice>)

template <typename T> static void fp_deleting(Footprint& f, T*, Edx, uint32_t) { f.replay_only = "destroys the device (and frees it)"; }
static void* __fastcall ILineDevice_deleting_dtor(LineDevice* self, Edx, uint32_t flags) {
    self->vtbl = (const void*)(uintptr_t)VT_ILineDevice;
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
static void fp_ILineDevice_deleting_dtor(Footprint& f, LineDevice* self, Edx, uint32_t flags) {
    if (flags & 1) f.replay_only = "frees the device";
    else f.add(self, 4, "the vtable");
}
PORT_FN(0x004a1ff0, "ILineDevice::vector deleting destructor", ILineDevice_deleting_dtor, fp_ILineDevice_deleting_dtor)
static void* __fastcall TAPILine_deleting_dtor(TAPILine* self, Edx, uint32_t flags) {
    tcall<void>(A_TAPILine_dtor, self);
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
PORT_FN(0x004a2010, "TAPILine::vector deleting destructor", TAPILine_deleting_dtor, fp_deleting<TAPILine>)
static void* __fastcall DirectLine_deleting_dtor(LineDevice* self, Edx, uint32_t flags) {
    tcall<void>(A_DirectLine_dtor, self);
    if (flags & 1) ccall<void>(F_op_delete, (void*)self);
    return self;
}
PORT_FN(0x004a2060, "DirectLine::vector deleting destructor", DirectLine_deleting_dtor, fp_deleting<LineDevice>)

#undef K32
#undef TAPI
}  // namespace net_line
}  // namespace
