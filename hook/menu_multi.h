// menu_multi.h -- multiplayer stage N3 (group A): the layouts of mmulti.obj's classes (the Multiplayer screen), its
// statics, and the game functions hook/menu_multi.cpp and test/world_menu_multi.cpp call. Also open to group B
// (hook/menu_sched.cpp): MultiGenesisInfo is what MenuMultiChooseTransport hands the lobby (MenuMultiScheduler).
//
// Recovered from the v1.0 disassembly: each size is the room MenuMultiChooseTransport's frame gives the object (it builds
// the NetBrowser and both line controls on its stack), static_assert'd. Every field is volatile: the rewrites read and
// write each one where the original does, in its order. The toolkit (UICustomControl, UIStringList, UIDialogItem) is
// hook/ui_types.h's; the item helpers (xl, once_xl, item_ctor, item_end, add_items, sty) hook/menu_options.h's.
//
//   NetBrowser 0x80 (vtable 0x4de9e0)        the LAN tab: the game list (one SessionMgr per protocol), Create / Find / Connect
//   LineControl 0x1f0 (vtable 0x4dea30)      a line tab's base: the devices (LineEnumerateDevices), the call, the line check
//   ModemLineControl (0x4dea80), DirectLineControl (0x4dead0): LineControl with their own Added (and the Modem's phone number)
//   MultiGenesisInfo 0x6c                    the choice made: the socket, server or client, the password, the service
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "port.h"
#include "ui_types.h"

namespace mmu {
using namespace uit;

// ---- the classes ----------------------------------------------------------------------------------------------------------
struct NetBrowser : UICustomControl {             // 0x80, vtable 0x4de9e0
    UIStringList list;                            // +0x18 the games found (0x20 entries of 0x40)
    volatile int32_t proto;                       // +0x2c 0 TCP/IP (UDP), 1 IPX (option MULTI net_protocol)
    void* volatile mgr[2];                        // +0x30 SessionMgr* per protocol (0 if its socket couldn't be made)
    volatile int32_t last_broadcast;              // +0x38 PTimeNow of the last ServiceRequestBroadcast
    volatile int32_t sel;                         // +0x3c the list's selection
    volatile uint32_t grp_find;                   // +0x40 the group of Find (TCP/IP only)
    volatile uint32_t grp_connect;                // +0x44 the group of Connect (and its Enter key): shown while games are listed
    char game_name[0x20];                         // +0x48 Create's name for the game
    char password[0x10];                          // +0x68 Create's password, or the one Connect asks for
    volatile int32_t timer;                       // +0x78 async_find_host's: PTimeNow + 2.5 s once the lookup ended
    uint8_t* volatile block;                      // +0x7c find_host's AsyncServiceRequestBlock (MemAlloc 0x438)
};
static_assert(sizeof(NetBrowser) == 0x80 && offsetof(NetBrowser, list) == 0x18 && offsetof(NetBrowser, proto) == 0x2c &&
              offsetof(NetBrowser, game_name) == 0x48 && offsetof(NetBrowser, block) == 0x7c, "NetBrowser");

struct LineDevInfo {                              // 0x50: LineEnumerateDevices' entries (net_types.h's LineDeviceInfo)
    volatile uint8_t flag0;                       // +0x00 TAPI: a modem (the call is "Connecting to <number>")
    volatile uint8_t in_use;                      // +0x01 in use by another application
    char name[0x40];                              // +0x02
    uint8_t _42[2];
    void* (__cdecl* volatile create)(void*);      // +0x44 CreateTAPILine / CreateDirectLine (given the entry)
    volatile int32_t id;                          // +0x48
    uint32_t _4c;
};
static_assert(sizeof(LineDevInfo) == 0x50 && offsetof(LineDevInfo, create) == 0x44, "LineDevInfo");

struct LineControl : UICustomControl {            // 0x1f0, vtable 0x4dea30 (Modem 0x4dea80, Direct 0x4dead0)
    volatile uint32_t grp_controls;               // +0x18 Call / number / Answer (Modem), Connect (Direct)
    char phone[0x20];                             // +0x1c the number to call (Modem; option MULTI phone_number)
    volatile uint32_t grp_none;                   // +0x3c "there are no devices"
    volatile uint32_t grp_in_use;                 // +0x40 "this device is in use"
    volatile uint8_t is_server;                   // +0x44 line_check's verdict (LineChecker::IsServer)
    volatile uint8_t kind;                        // +0x45 the constructor's argument: 1 modems, 2 COM ports
    uint8_t _46[2];
    volatile int32_t sel;                         // +0x48 the device chosen (radio buttons)
    volatile int32_t count;                       // +0x4c devices found (5 at most)
    LineDevInfo dev[5];                           // +0x50
    void* volatile device;                        // +0x1e0 ILineDevice* (the chosen entry's create)
    char* volatile msg;                           // +0x1e4 the cancel box's message (a caller's stack buffer, or 0)
    volatile int32_t timer;                       // +0x1e8 PTimeNow + 2.5 s: the box closes then
    void* volatile checker;                       // +0x1ec LineChecker* (line_check's stack)
};
static_assert(sizeof(LineControl) == 0x1f0 && offsetof(LineControl, dev) == 0x50 && offsetof(LineControl, device) == 0x1e0,
              "LineControl");

struct MultiGenesisInfo {                         // 0x6c (its constructor zeroes all of it)
    void* volatile sock;                          // +0x00 ISocket*
    volatile uint8_t is_server;                   // +0x04
    volatile uint8_t named;                       // +0x05 1: a new game or a line (the service below is filled)
    char password[0x12];                          // +0x06
    uint8_t service[0x54];                        // +0x18 RemoteService: the game joined, or (a new game) its name
};
static_assert(sizeof(MultiGenesisInfo) == 0x6c && offsetof(MultiGenesisInfo, service) == 0x18, "MultiGenesisInfo");

// ---- vtables ----------------------------------------------------------------------------------------------------------------
enum : uint32_t {
    VT_CustomControl = 0x004db190, VT_NetBrowser = 0x004de9e0, VT_LineControl = 0x004dea30,
    VT_ModemLineControl = 0x004dea80, VT_DirectLineControl = 0x004dead0,
};

// ---- the game's functions (v1.0) -------------------------------------------------------------------------------------------
enum : uint32_t {
    // kernel, gx, ui, the CRT
    F_Win32Idle = 0x00412bf0, F_gxTextWidth = 0x00453c80, F_UIDoOkBox = 0x004793c0, F_UIDoInputBox = 0x0047a3d0,
    F_atoi = 0x004cf990,
    // multi
    F_LineDeviceInfo_GetError = 0x004a1130, F_LineEnumerateDevices = 0x004a1690, F_LineStatusText = 0x004a1bc0,
    F_SessionMgr_dtor = 0x004a4f40, F_SessionMgr_CanDestroySafely = 0x004a4fc0, F_SessionMgr_AsyncCancel = 0x004a5060,
    F_SessionMgr_Tick = 0x004a50d0, F_SessionMgr_GetServiceTable = 0x004a55c0,
    F_SessionMgr_ServiceRequestBroadcast = 0x004a5620, F_SessionMgr_AsyncServiceRequestByHostname = 0x004a57b0,
    F_CreateSessionMgr = 0x004a7670, F_SocketCreateLine = 0x004ae3f0, F_LineChecker_ctor = 0x004ae9b0,
    F_LineChecker_LineOk = 0x004ae9f0, F_LineChecker_Tick = 0x004aea10, F_LineChecker_DoneChecking = 0x004aec30,
    F_LineChecker_IsServer = 0x004aec60,
    // mmulti.obj (called by address, as the original does, so a hooked rewrite is what runs)
    F_new_server = 0x0048c8c0, F_MyDoInputBoxN = 0x0048c9f0, F_FindHost = 0x0048ce20, F_async_find_host = 0x0048cf50,
    F_find_host = 0x0048cfa0, F_MyDoCancelBox = 0x0048d0e0, F_cb_idle_func = 0x0048d280, F_NetBrowser_ctor = 0x0048d2c0,
    F_CreateSocket = 0x0048d360, F_DestroySockets = 0x0048d380, F_NetBrowser_dtor = 0x0048d400, F_NetBrowser_Tick = 0x0048d440,
    F_reset_active_proto = 0x0048d610, F_connect = 0x0048d710, F_LC_Checking = 0x0048dc90, F_LC_Connecting = 0x0048dca0,
    F_LC_Answering = 0x0048dcb0, F_DLC_Connect = 0x0048dcc0, F_MLC_Answer = 0x0048dcf0, F_MLC_Call = 0x0048dd20,
    F_LineControl_ctor = 0x0048dd50, F_LineControl_dtor = 0x0048ddb0, F_LC_disconnect = 0x0048ddc0,
    F_LC_GetDevice = 0x0048ddd0, F_LC_line_check = 0x0048ddf0, F_LC_destroy_device = 0x0048dee0, F_LC_answering = 0x0048df40,
    F_LC_connecting = 0x0048dfe0, F_LC_update_check_msg = 0x0048e070, F_LC_checking = 0x0048e150,
    F_LC_place_call = 0x0048e310, F_LC_answer = 0x0048e560, F_LC_add_line_devices = 0x0048e7e0, F_LC_Added = 0x0048e8b0,
    F_MenuMultiChooseTransport = 0x0048f060, F_next_tab_cb = 0x0048f910, F_prev_tab_cb = 0x0048f950,
    F_ASyncFindHost = 0x0048fa10, F_NewServer = 0x0048fa40, F_NB_Connect = 0x0048fa50,
};

// ---- statics (v1.0) --------------------------------------------------------------------------------------------------------
enum : uint32_t {
    S_SEC_MULTI = 0x004de9d8,                     // char*: "MULTI"
    S_MULTI_VERSION = 0x004df354,                 // int const MULTI_VERSION (0x25)
    S_CREATE_SOCKET = 0x004f8b20,                 // ISocket* (*)(short)[2]: SocketCreateUDP, SocketCreateIPX
    S_NB_GLOBAL = 0x0057a450,                     // NetBrowser::global
    S_LC_GLOBAL = 0x0057a344,                     // LineControl::global (place_call / answer set it)
    S_DLC_GLOBAL = 0x0057a470,                    // DirectLineControl::global
    S_MLC_GLOBAL = 0x0057a434,                    // ModemLineControl::global
    S_CB_IDLE = 0x0057a388,                       // MyDoCancelBox's idle function (cb_idle_func calls it)
    S_TAB = 0x0057a38c,                           // the tab chosen (option MULTI main_tab): one of the three group ids
    S_GRP_LAN = 0x0057a3c4, S_GRP_DIRECT = 0x0057a3f8, S_GRP_MODEM = 0x0057a348,   // the tabs' groups
    // colours (the $E initialisers')
    S_COL_4FC = 0x0057a4fc, S_COL_40C = 0x0057a40c, S_COL_56C = 0x0057a56c,
};

}  // namespace mmu
