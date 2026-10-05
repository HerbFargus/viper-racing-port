// krn_util.cpp -- M3 stage "kernel and utilities", group K3, rewritten: the keyboard and mouse queues (key.obj,
// mouse.obj), what M2 left of scan.obj and joy.obj, Marsaglia's random generator (random.obj), the pools
// (pool.obj: PoolBase / DebugPoolBase), the node pool (node.obj), bags (bag.obj), memory streams (mstream.obj),
// string tables (table.obj), base64 (base64.obj), Slerp (slerp.obj), the HTML writer (html.obj), the clipboard
// (clpboard.obj) and useful.obj's begin / end.
//
// Written from the v1.0 disassembly (tools/disasm.py): every call in the original's order, by its v1.0 address
// (this group's own functions too, so the hooked rewrite -- or the recorder's hook in front of Random and
// Randomize -- is what runs), Windows through the game's own import slots, the C runtime (stricmp, strncpy,
// vsprintf, _CIfmod) at its game address, memory moved with integer instructions kept as bit copies (rep movsd /
// movsb forwards, in the same pieces), register values as double and stored values as float.
//
// Threads. The key and mouse queues are filled and read on the main thread (Win32 messages, M2's SDL events). The
// random generator is drawn from both threads (the physics' Random(int), the menus and the AI's set-up); it has its
// own lock (a Multi), and its statics are listed by every footprint that writes them. The node pool is shared too
// (a Multi). Nothing here is among the automatically saved physics/AI statics, so each footprint lists the
// statics its function writes.
//
// Quirks kept (faithful; see the fix candidates in the harness's notes and the report):
//   * KeyGet / queue take and drop the key lock before touching the queue; MousePeekEvent pumps the window's
//     messages (Win32Idle) inside the mouse lock.
//   * the key queue holds 31 characters (32 slots, one kept empty), the mouse queue 15 events; a full key queue
//     logs "key.cpp: overflow" and drops the key, a full mouse queue drops the event silently.
//   * StringTableSort's swap writes the saved row back into the SAME row (row a = row b; row a = saved a): it
//     never changes the table. StringTableFind's binary search stops when the step reaches 0, so it never looks
//     at row 0 and misses others. StringTableGet takes a non-zero "version" as a wrong table.
//   * Slerp negates its const input `a` in place when the quaternions' dot product is negative (or NaN).
//   * base64 uses '#' and '$' for 62 and 63, has no '=' padding (a padded string fails), and from_64 indexes its
//     table with a signed char (bytes >= 0x80 read the 128 bytes before it).
//   * MemStreamGetString ignores its buffer size and reads an uninitialised length when the stream is exhausted.
//   * ClipboardGetText passes the locked pointer (not the handle) to GlobalUnlock.
//
// MemStreamGetString is the one rewrite in assembly: it reads an uninitialised stack slot (when the stream is
// exhausted), which only the original's own frame layout reproduces.
//
// Skipped: the 30 $E1/$E2 static initialisers (they run in the C runtime's start-up, before anything is hooked,
// and only jump to rcfunc_is_internal). HTMLWriteLn / HTMLWrite are C variadic functions, which PORT_FN's shadow
// wrapper can't take: they're registered with a fixed signature of 16 dwords after the format, and build their
// va_list from the first (__cdecl on x86: the caller's arguments lie in order on the stack, so a rewrite running
// in place of the original reads exactly what the original would; a shadow check copies 16 dwords -- enough for
// any format the game uses). They are replay_only anyway (they write files).
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#if !defined(VP_FAITHFUL) && !defined(VP_FUZZ)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif
#include "port.h"
#include "x87.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static __forceinline float Fb(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static __forceinline uint32_t Ub(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static __forceinline double Db(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

// rep movsd (n >> 2 dwords) then rep movsb (n & 3 bytes), forwards: the original's inlined memcpy / strcpy. The
// count is shifted logically, as the original does (a negative n is a huge copy there too).
static __forceinline void rep_movs(void* dst, const void* src, uint32_t n) {
    __asm { mov edi, dst
            mov esi, src
            mov ecx, n
            mov edx, ecx
            shr ecx, 2
            rep movsd
            mov ecx, edx
            and ecx, 3
            rep movsb }
}

#define G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define G16(a) (*(volatile uint16_t*)(uintptr_t)(a))
#define G32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define GU32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define GF(a) (*(volatile float*)(uintptr_t)(a))
#define GP(T, a) (*(T* volatile*)(uintptr_t)(a))
#define FP_ADD(f, addr, n, what) (f).add((void*)(uintptr_t)(addr), (uint32_t)(n), (what))
#define IAT(T, slot) (*(T*)(uintptr_t)(slot))           // a Windows function through the game's own import slot

namespace {

// ---- layouts ------------------------------------------------------------------------------------------------------
struct PoolBase {                         // 0x20, vtable 0x4db4f0 (Pool<Node> 0x4db478, Pool<block> 0x4db4f4)
    void* vtbl;
    uint8_t crashes, _p[3];               // +4 OverallocCrashes: panic when empty (else alloc returns 0)
    const char* name;                     // +8
    uint8_t* mem;                         // +0xc the block
    void* head;                           // +0x10 the free list
    uint32_t size;                        // +0x14 element size
    int32_t count;                        // +0x18 elements
    int32_t used;                         // +0x1c
};
static_assert(sizeof(PoolBase) == 0x20, "PoolBase");
struct DebugBlock { void* mem; DebugBlock* next; };
struct DebugPoolBase {                    // 0x3c, vtable 0x4db4f8
    void* vtbl;
    uint8_t crashes, _p[3];               // +4
    const char* name;                     // +8
    int32_t max;                          // +0xc
    int32_t used;                         // +0x10
    uint32_t size;                        // +0x14
    DebugBlock* blocks;                   // +0x18
    PoolBase pool;                        // +0x1c Pool<DebugPoolBase::block> ("debugpool's pool", 8-byte blocks)
};
static_assert(sizeof(DebugPoolBase) == 0x3c, "DebugPoolBase");
struct BagBase { void** items; const char* name; int32_t count, cap; uint8_t crashes, _p[3]; };
static_assert(sizeof(BagBase) == 0x14, "BagBase");
struct MemStream { uint8_t* data; int32_t size; };
struct MemStreamPtr { MemStream* s; int32_t pos; uint8_t overflow, _p[3]; };
static_assert(sizeof(MemStreamPtr) == 0xc, "MemStreamPtr");
struct StringTable { int32_t rows, cols; int32_t coloff[16]; int32_t rowsize; char data[4]; };   // data at +0x4c
static_assert(offsetof(StringTable, data) == 0x4c, "StringTable");
struct MouseEvent { int32_t type, x, y, state; };
struct HTMLStyle { const char* open; const char* close; };

// ---- the game's functions (by v1.0 address) ------------------------------------------------------------------------
typedef void(__cdecl* Void_t)();
typedef int(__cdecl* SingleBegin_t)(const char*);
typedef void(__cdecl* SingleOp_t)(int, const char*, int);
static const SingleBegin_t SingleBegin = (SingleBegin_t)0x00414f30;
static const SingleOp_t SingleEnter = (SingleOp_t)0x00415000, SingleLeave = (SingleOp_t)0x00415070,
                        SingleEnd = (SingleOp_t)0x00415090;
static const SingleBegin_t MultiBegin = (SingleBegin_t)0x004150a0;
static const SingleOp_t MultiEnter = (SingleOp_t)0x00415180, MultiLeave = (SingleOp_t)0x004151d0,
                        MultiEnd = (SingleOp_t)0x00415220;
static const Void_t Win32Idle = (Void_t)0x00412bf0;
typedef void*(__cdecl* Win32GetWindow_t)();
static const Win32GetWindow_t Win32GetWindow = (Win32GetWindow_t)0x00412be0;
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogReport = (Log_t)0x00411150, LogPanic = (Log_t)0x004112b0;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* MemFree_t)(void*);
static const MemFree_t MemFree = (MemFree_t)0x00414300, OperatorDelete = (MemFree_t)0x00414390;
typedef int(__cdecl* PTimeNow_t)();
static const PTimeNow_t PTimeNow = (PTimeNow_t)0x00413b40;
typedef uint8_t(__cdecl* FileIO_t)(int, void*, int);
static const FileIO_t FileWrite = (FileIO_t)0x00411a30, FileReadExact = (FileIO_t)0x004118b0;
typedef int(__cdecl* FileCreate_t)(const char*);
static const FileCreate_t FileCreate = (FileCreate_t)0x004115f0;
typedef void(__cdecl* FileClose_t)(volatile int32_t*);
static const FileClose_t FileClose = (FileClose_t)0x00411850;
typedef void(__cdecl* FilePrintf_t)(int, char*, const char*, ...);
static const FilePrintf_t FilePrintf = (FilePrintf_t)0x00411b40;
typedef void*(__cdecl* ResourceGet_t)(const char*, uint32_t, uint32_t*, int*, uint8_t*, uint8_t*);
static const ResourceGet_t ResourceGet = (ResourceGet_t)0x00419fa0;
typedef uint8_t(__cdecl* ResourceForget_t)(void*);
static const ResourceForget_t ResourceForget = (ResourceForget_t)0x0041a450;
typedef uint8_t(__cdecl* ResourceBegin_t)(const char*, const char*);
static const ResourceBegin_t ResourceBegin = (ResourceBegin_t)0x00419640;
static const Void_t ResourceEnd = (Void_t)0x0041a5a0;
// the game's C runtime
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t game_stricmp = (Stricmp_t)0x004da350;
typedef char*(__cdecl* Strncpy_t)(char*, const char*, int);
static const Strncpy_t game_strncpy = (Strncpy_t)0x004cf3a0;
typedef int(__cdecl* Vsprintf_t)(char*, const char*, va_list);
static const Vsprintf_t game_vsprintf = (Vsprintf_t)0x004cf7c0;
static const uint32_t k_CIfmod = 0x004cf36a;         // _CIfmod: fmod(ST1, ST0), on the FPU stack
// Windows, through the game's import slots
typedef uint32_t(__stdcall* MapVirtualKeyA_t)(uint32_t, uint32_t);
typedef int(__stdcall* OpenClipboard_t)(void*);
typedef void*(__stdcall* GetClipboardData_t)(uint32_t);
typedef void*(__stdcall* GlobalLock_t)(void*);
typedef int(__stdcall* GlobalUnlock_t)(void*);
typedef int(__stdcall* CloseClipboard_t)();
typedef int(__stdcall* EmptyClipboard_t)();
typedef void*(__stdcall* GlobalAlloc_t)(uint32_t, uint32_t);
typedef void*(__stdcall* SetClipboardData_t)(uint32_t, void*);
enum : uint32_t {
    IAT_MapVirtualKeyA = 0x005d769c, IAT_OpenClipboard = 0x005d7680, IAT_GetClipboardData = 0x005d76d0,
    IAT_GlobalLock = 0x005d7560, IAT_GlobalUnlock = 0x005d759c, IAT_CloseClipboard = 0x005d7684,
    IAT_EmptyClipboard = 0x005d768c, IAT_GlobalAlloc = 0x005d755c, IAT_SetClipboardData = 0x005d7688,
};
// a COM method: the object's vtable slot, __stdcall with the object first
#define COM(obj, off, R, ...) ((R(__stdcall*)(void*, ##__VA_ARGS__))((*(void***)(obj))[(off) / 4]))

// ---- this group's own functions, called by address ------------------------------------------------------------------
typedef uint8_t(__cdecl* U8_t)();
typedef uint16_t(__cdecl* U16_t)();
typedef int32_t(__cdecl* IntInt_t)(int32_t);
typedef void(__cdecl* Queue_t)(uint32_t, uint32_t);   // queue(unsigned short, unsigned char): pushed as dwords
typedef void(__cdecl* AddToBuffer_t)(uint32_t);
typedef uint8_t(__cdecl* IsMetaChar_t)(uint32_t);
static const Void_t KeyClear = (Void_t)0x00413c50;
static const U16_t buffer_get = (U16_t)0x00413fe0;
static const U8_t is_buffer_empty = (U8_t)0x00413f30, is_buffer_full = (U8_t)0x00413f90;
static const Queue_t queue = (Queue_t)0x00413ee0;
static const AddToBuffer_t add_to_buffer = (AddToBuffer_t)0x00413f50;
static const IntInt_t next_element = (IntInt_t)0x00413fc0;
static const IsMetaChar_t is_meta_char = (IsMetaChar_t)0x00414010;
static const Void_t MouseClear = (Void_t)0x00414470;
static const U8_t mouse_is_buffer_empty = (U8_t)0x00414670, mouse_is_buffer_full = (U8_t)0x00414730;
typedef uint8_t(__cdecl* MouseEv_t)(MouseEvent*);
static const MouseEv_t mouse_buffer_get = (MouseEv_t)0x00414690, mouse_buffer_add = (MouseEv_t)0x004146e0;
typedef MouseEvent*(__cdecl* MouseTop_t)();
static const MouseTop_t mouse_buffer_top = (MouseTop_t)0x00414760;
typedef const char*(__cdecl* DiError_t)(int32_t);
static const DiError_t di_error = (DiError_t)0x00413120;
typedef int32_t(__cdecl* SetPropDword_t)(const void*, uint32_t, uint32_t);
static const SetPropDword_t set_prop_dword = (SetPropDword_t)0x004194e0;
static const Void_t RandomBegin = (Void_t)0x0041b670, RandomEnd = (Void_t)0x0041b690, Randomize = (Void_t)0x0041b6b0;
static const IntInt_t Random = (IntInt_t)0x0041b6e0;
typedef void(__cdecl* Rmarin_t)(int32_t, int32_t);
static const Rmarin_t rmarin = (Rmarin_t)0x0041b720;
typedef float(__cdecl* Ranmar_t)();
static const Ranmar_t ranmar = (Ranmar_t)0x0041b910;
typedef void(__cdecl* NodeBegin_t)(int32_t);
static const NodeBegin_t NodeBegin = (NodeBegin_t)0x0041b460;
static const Void_t NodeEnd = (Void_t)0x0041b4d0;
typedef PoolBase*(__fastcall* PoolCtor_t)(PoolBase*, Edx, const char*, int32_t, uint32_t);
static const PoolCtor_t PoolBase_ctor = (PoolCtor_t)0x0041ba30;
typedef void(__fastcall* PoolVoid_t)(void*, Edx);
static const PoolVoid_t PoolBase_init_list = (PoolVoid_t)0x0041ba90, PoolBase_dtor = (PoolVoid_t)0x0041bac0,
                        PoolBase_freeall = (PoolVoid_t)0x0041bb80, DebugPoolBase_dtor = (PoolVoid_t)0x0041bbe0;
typedef void*(__fastcall* PoolAlloc_t)(void*, Edx);
static const PoolAlloc_t PoolBase_alloc = (PoolAlloc_t)0x0041bb20;
typedef void(__fastcall* PoolFree_t)(void*, Edx, void*);
static const PoolFree_t PoolBase_free = (PoolFree_t)0x0041bb60;
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG_pool = (AssertMsg_t)0x0041bb00;
typedef uint32_t(__cdecl* B64Triple_t)(const char*);
static const B64Triple_t base64_to_triple = (B64Triple_t)0x0041b0c0;
typedef uint8_t(__cdecl* From64_t)(uint32_t);
static const From64_t from_64 = (From64_t)0x0041b100;
typedef void(__cdecl* TripleTo64_t)(char*, uint32_t);
static const TripleTo64_t triple_to_base64 = (TripleTo64_t)0x0041b1b0;
typedef char(__cdecl* To64_t)(uint32_t);
static const To64_t to_64 = (To64_t)0x0041b1e0;
typedef const char*(__cdecl* GetEntry_t)(const StringTable*, int32_t, int32_t);
static const GetEntry_t StringTableGetEntry = (GetEntry_t)0x0041b2a0;
typedef void(__cdecl* MsPutInt_t)(MemStreamPtr*, int32_t);
static const MsPutInt_t MemStreamPutInt = (MsPutInt_t)0x004d8ce0;
typedef void(__cdecl* MsData_t)(MemStreamPtr*, const void*, int32_t);
static const MsData_t MemStreamPutData = (MsData_t)0x004d8d80;
typedef void(__cdecl* MsGetInt_t)(MemStreamPtr*, int32_t*);
static const MsGetInt_t MemStreamGetInt = (MsGetInt_t)0x004d8dc0;
typedef void(__cdecl* MsGetData_t)(MemStreamPtr*, void*, int32_t);
static const MsGetData_t MemStreamGetData = (MsGetData_t)0x004d8e50;
typedef void(__cdecl* HTMLWriteLn_t)(const char*, ...);
static const HTMLWriteLn_t HTMLWriteLn = (HTMLWriteLn_t)0x004d9a60;

// ---- the statics ----------------------------------------------------------------------------------------------------
enum : uint32_t {
    // scan.obj
    A_SCAN = 0x00508088,                  // uint8[256]: bit 7 down, bit 0 hit
    A_DI = 0x00508188, A_DIDEV = 0x0050818c,
    // key.obj
    A_KEY_SINGLE = 0x005082e8, A_KEY_META = 0x005082ec /* uint16 */, A_KEY_BUF = 0x005082f0 /* uint16[32] */,
    A_KEY_OFF = 0x00508330 /* uint8[0x200]: a key's characters are dropped */, A_KEY_HEAD = 0x00508530,
    A_KEY_TAIL = 0x00508534,
    // mouse.obj
    A_MOUSE_X = 0x00508548, A_MOUSE_Y = 0x0050854c, A_MOUSE_BUF = 0x00508550 /* MouseEvent[16] */,
    A_MOUSE_STATE = 0x00508650, A_MOUSE_SINGLE = 0x00508654, A_MOUSE_HEAD = 0x00508658, A_MOUSE_TAIL = 0x00508660,
    // joy.obj
    A_JOY_NAME = 0x00508f08, A_JOY_DEV = 0x00509108, A_JOY_DI = 0x0050910c, A_JOY_EFFECT_X = 0x00509114,
    A_JOY_EFFECT_Y = 0x00509118, A_JOY_DEV2 = 0x0050911c,
    // base64.obj
    A_B64_ALPHABET = 0x004eb400, A_B64_INIT = 0x004eb444 /* uint8: the table is still to be built */,
    A_B64_TABLE = 0x00509660 /* uint8[256] */,
    // node.obj
    A_NODE_MULTI = 0x00509760, A_NODE_POOL = 0x004eb498, A_NODE_MAX = 0x004eb49c, A_NODE_GRABBED = 0x004eb4a0,
    // random.obj: Marsaglia's generator
    A_RAN_C = 0x00509768, A_RAN_U = 0x00509770 /* float[97] */, A_RAN_CD = 0x005098f4, A_RAN_CM = 0x005098f8,
    A_RAN_MULTI = 0x005098fc, A_RAN_I97 = 0x00509904, A_RAN_J97 = 0x00509908, A_RAN_TEST = 0x004eb4f8,
    A_RAN_FIRST = 0x00509768, RAN_BYTES = 0x0050990c - 0x00509768,
    // html.obj
    A_HTML_FILE = 0x00503dd4,
};

// ---- strings and constant data (the original's addresses) -------------------------------------------------------------
static const char* const k_str_key = (const char*)0x004e6570;              // "key"
static const char* const k_str_key_overflow = (const char*)0x004e6574;     // "key.cpp: overflow"
static const char* const k_str_mouse = (const char*)0x004e668c;            // "mouse"
static const char* const k_fmt_no_table = (const char*)0x004eb448;         // "Can't find string table %s"
static const char* const k_fmt_table_version = (const char*)0x004eb464;    // "Wrong string table version %x (expecting %x) in %s"
static const char* const k_str_node = (const char*)0x004eb4a4;             // "node"
static const char* const k_str_nodes = (const char*)0x004eb4b8;            // "nodes"
static const char* const k_str_node_reserved = (const char*)0x004eb4c0;    // "Too many nodes reserved!"
static const char* const k_str_node_released = (const char*)0x004eb4dc;    // "Too many nodes released!"
static const char* const k_str_random = (const char*)0x004eb4fc;           // "random"
static const char* const k_str_pool_nomem = (const char*)0x004eb504;       // "Unable to allocate memory for pool"
static const char* const k_fmt_pool_destroyed = (const char*)0x004eb528;   // "Pool "%s" was destroyed before all blocks were freed."
static const char* const k_fmt_pool_oom = (const char*)0x004eb560;         // "Panic: out of memory in pool(%s)"
static const char* const k_str_debugpool_pool = (const char*)0x004eb584;   // "debugpool's pool"
static const char* const k_fmt_dpool_destroyed = (const char*)0x004eb598;  // "Pool "%s" was destroyed before all elements were freed"
static const char* const k_fmt_overalloc = (const char*)0x004eb5d0;        // "Overallocating from pool "%s""
static const char* const k_fmt_dpool_oom = (const char*)0x004eb5f0;        // "Panic: out of memory in pool(%s)"
static const char* const k_fmt_dpool_bad_free = (const char*)0x004eb614;   // "Pool "%s" : trying to free unrecognized pointer!"
static const char* const k_fmt_bag_full = (const char*)0x00503d98;         // "Ran out of space in bag %s"
static const char* const k_fmt_bag_unknown = (const char*)0x00503db4;      // "Bag %s: removing unknown object"
static const char* const k_fmt_html_head = (const char*)0x00503e24;        // "<HTML><TITLE>%s</TITLE></HEAD>"
static const char* const k_str_html_body = (const char*)0x00503e44;        // "<BODY>"
static const char* const k_fmt_html_cant = (const char*)0x00503e4c;        // "HTML: Can't create %s"
static const char* const k_str_html_end = (const char*)0x00503e64;         // "</BODY></HTML>"
static const char* const k_fmt_html_line = (const char*)0x00503e74;        // "%s\r\n"
static const char* const k_fmt_html_styled = (const char*)0x00503e7c;      // "%s%s%s\r\n"
static const char* const k_str_dot1 = (const char*)0x004eae34;             // "."
static const char* const k_str_dot2 = (const char*)0x004eae38;             // "."
static const char* const k_fmt_joystick = (const char*)0x004eac74;         // "Joystick: "%s""
static const char* const k_str_sidewinder_ffb = (const char*)0x004eac84;   // "Microsoft SideWinder Force Feedback Wheel"
static const char* const k_str_fakes_y = (const char*)0x004eacb0;          // "Controller fakes y-axis"
static const char* const k_str_autocenter_off = (const char*)0x004eacc8;   // "Turned off autocenter"
static const char* const k_str_x_effect = (const char*)0x004eace0;         // "Created x-effect"
static const char* const k_fmt_x_effect_fail = (const char*)0x004eacf4;    // "Couldn't create x effect (%s)"
static const char* const k_str_x_unsupported = (const char*)0x004ead14;    // "x-effect unsupported"
static const char* const k_fmt_y_effect_fail = (const char*)0x004ead2c;    // "Couldn't create y effect (%s)"
static const char* const k_str_no_di2 = (const char*)0x004ead4c;           // "Couldn't get DirectInput2 joystick"
static const char* const k_str_no_coop = (const char*)0x004ead70;          // "Couldn't set joystick coop level"
static const char* const k_str_no_format = (const char*)0x004ead94;        // "Couldn't set joystick format"
static const char* const k_str_no_device = (const char*)0x004eadb4;        // "Couldn't create joystick device"
static const void* const k_c_dfDIJoystick = (const void*)0x004cdfe0;
static const void* const k_IID_IDirectInputDevice2 = (const void*)0x004dce10;
static const void* const k_GUID_ConstantForce = (const void*)0x004dcf20;
static const void* const k_vt_PoolBase = (const void*)0x004db4f0;
static const void* const k_vt_PoolBlock = (const void*)0x004db4f4;         // Pool<DebugPoolBase::block>
static const void* const k_vt_DebugPoolBase = (const void*)0x004db4f8;
static const void* const k_vt_PoolNode = (const void*)0x004db478;          // Pool<Node>
static const uint32_t k_di_unknown = 0x004e6414;                           // "Unknown"
static const uint32_t E_NOTIMPL_ = 0x80004001u;

static void fp_none(Footprint&) {}

}  // namespace

// =====================================================================================================================
// scan.obj (ScanBegin / ScanUpdate are M2's, platform.cpp)
// =====================================================================================================================

// ScanEnd (0x412ff0): the keyboard device unacquired and released, then DirectInput released
static void __cdecl ScanEnd_rw() {
    if (GP(void, A_DIDEV)) {
        void* d = GP(void, A_DIDEV);
        COM(d, 0x20, long)(d);                              // Unacquire
        d = GP(void, A_DIDEV);
        COM(d, 0x08, unsigned long)(d);                     // Release
    }
    GP(void, A_DIDEV) = 0;
    if (GP(void, A_DI)) {
        void* di = GP(void, A_DI);
        COM(di, 0x08, unsigned long)(di);                   // Release
    }
    GP(void, A_DI) = 0;
}
static void fp_scan_end(Footprint& f) { f.replay_only = "it releases the DirectInput keyboard"; }
PORT_FN(0x00412ff0, "ScanEnd", ScanEnd_rw, fp_scan_end)

// ScanDown (0x4130e0): bit 7 of the key's state
static uint8_t __cdecl ScanDown_rw(uint8_t k) { return (uint8_t)((G8(A_SCAN + k) & 0x80) >> 7); }
static void fp_scan_down(Footprint&, uint8_t) {}
PORT_FN(0x004130e0, "ScanDown", ScanDown_rw, fp_scan_down)

// ScanHit (0x413100): bit 0 of the key's state, cleared as it's read
static uint8_t __cdecl ScanHit_rw(uint8_t k) {
    uint8_t v = G8(A_SCAN + k);
    if (!(v & 1)) return 0;
    G8(A_SCAN + k) = (uint8_t)(v & 0xfe);
    return 1;
}
static void fp_scan_hit(Footprint& f, uint8_t k) { FP_ADD(f, A_SCAN + k, 1, "scan state"); }
PORT_FN(0x00413100, "ScanHit", ScanHit_rw, fp_scan_hit)

// di_error (0x413120): a DirectInput HRESULT's name (the strings' addresses; the original's compare tree and jump
// tables, flattened)
static const char* __cdecl di_error_rw(int32_t hr) {
    uint32_t s;
    switch ((uint32_t)hr) {
    case 0x8000000a: s = 0x004e6408; break;         // E_PENDING
    case 0x80004001: s = 0x004e63f4; break;         // DIERR_UNSUPPORTED (E_NOTIMPL)
    case 0x80004002: s = 0x004e6330; break;
    case 0x80004005: s = 0x004e62ac; break;
    case 0x80040110: s = 0x004e631c; break;
    case 0x80040154: s = 0x004e6270; break;
    case 0x80040201: s = 0x004e625c; break;
    case 0x80040202: s = 0x004e630c; break;
    case 0x80040203: s = 0x004e636c; break;
    case 0x80040204: s = 0x004e6298; break;
    case 0x80040205: s = 0x004e6380; break;
    case 0x80040206: s = 0x004e62d0; break;
    case 0x80040207: s = 0x004e6358; break;
    case 0x80040208: s = 0x004e6284; break;
    case 0x80070002: s = 0x004e639c; break;
    case 0x80070005: s = 0x004e62bc; break;
    case 0x8007000c: s = 0x004e6344; break;
    case 0x8007000e: s = 0x004e63e0; break;
    case 0x80070015: s = 0x004e63ac; break;
    case 0x8007001e: s = 0x004e62e8; break;
    case 0x80070057: s = 0x004e62f8; break;
    case 0x80070077: s = 0x004e6228; break;
    case 0x800700aa: s = 0x004e61fc; break;
    case 0x8007047e: s = 0x004e63c4; break;
    case 0x80070481: s = 0x004e623c; break;
    case 0x800704df: s = 0x004e620c; break;
    case 0: s = 0x004e61b8; break;
    case 1: s = 0x004e617c; break;
    case 2: s = 0x004e61c0; break;
    case 3: s = 0x004e6190; break;
    case 4: s = 0x004e61a4; break;
    case 8: s = 0x004e61d0; break;
    case 12: s = 0x004e61e0; break;
    default: s = k_di_unknown; break;
    }
    return (const char*)(uintptr_t)s;
}
static void fp_di_error(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x00413120, "di_error", di_error_rw, fp_di_error)

// =====================================================================================================================
// key.obj: the character queue (32 uint16 slots, one kept empty), the meta bits, the keys whose characters are dropped
// =====================================================================================================================

// KeyBegin (0x413bf0)
static void __cdecl KeyBegin_rw() {
    const int h = SingleBegin(k_str_key);
    G32(A_KEY_SINGLE) = h;
    G16(A_KEY_META) = 0;
    G32(A_KEY_HEAD) = 0;
    G32(A_KEY_TAIL) = 0;
    memset((void*)(uintptr_t)A_KEY_OFF, 0, 0x200);            // rep stosd, 0x80 dwords
}
static void fp_key_begin(Footprint& f) { f.replay_only = "it makes the key queue's lock (SingleBegin)"; }
PORT_FN(0x00413bf0, "KeyBegin", KeyBegin_rw, fp_key_begin)

// KeyEnd (0x413c30)
static void __cdecl KeyEnd_rw() {
    KeyClear();
    SingleEnd(G32(A_KEY_SINGLE), 0, 0);
}
static void fp_key_end(Footprint& f) { f.replay_only = "it pumps the window's messages (KeyClear) and ends the key lock"; }
PORT_FN(0x00413c30, "KeyEnd", KeyEnd_rw, fp_key_end)

// KeyClear (0x413c50): the pending messages first, then the queue emptied under the lock
static void __cdecl KeyClear_rw() {
    Win32Idle();
    SingleEnter(G32(A_KEY_SINGLE), 0, 0);
    const int h = G32(A_KEY_SINGLE);
    G32(A_KEY_HEAD) = 0;
    G32(A_KEY_TAIL) = 0;
    SingleLeave(h, 0, 0);
}
static void fp_key_clear(Footprint& f) { f.replay_only = "Win32Idle pumps the window's messages"; }
PORT_FN(0x00413c50, "KeyClear", KeyClear_rw, fp_key_clear)

// KeyGet (0x413c90): the lock taken and dropped, then the next character (0 if none)
static uint16_t __cdecl KeyGet_rw() {
    SingleEnter(G32(A_KEY_SINGLE), 0, 0);
    SingleLeave(G32(A_KEY_SINGLE), 0, 0);
    return buffer_get();
}
static void fp_key_get(Footprint& f) { FP_ADD(f, A_KEY_TAIL, 4, "key tail"); }
PORT_FN(0x00413c90, "KeyGet", KeyGet_rw, fp_key_get)

// KeyHit (0x413cc0)
static uint8_t __cdecl KeyHit_rw() {
    SingleEnter(G32(A_KEY_SINGLE), 0, 0);
    SingleLeave(G32(A_KEY_SINGLE), 0, 0);
    return is_buffer_empty() == 0 ? 1 : 0;                  // cmp al, 1; sbb al, al; neg al
}
PORT_FN(0x00413cc0, "KeyHit", KeyHit_rw, fp_none)

// KeyEnable / KeyDisable (0x413cf0 / 0x413d40): a key's characters let through / dropped; with bit 7 set, the
// key without it too
static void __cdecl KeyEnable_rw(uint8_t c) {
    SingleEnter(G32(A_KEY_SINGLE), 0, 0);
    SingleLeave(G32(A_KEY_SINGLE), 0, 0);
    if (c & 0x80) G8(A_KEY_OFF + (c & 0x7fu)) = 0;
    G8(A_KEY_OFF + c) = 0;
}
static void __cdecl KeyDisable_rw(uint8_t c) {
    SingleEnter(G32(A_KEY_SINGLE), 0, 0);
    SingleLeave(G32(A_KEY_SINGLE), 0, 0);
    if (c & 0x80) G8(A_KEY_OFF + (c & 0x7fu)) = 1;
    G8(A_KEY_OFF + c) = 1;
}
static void fp_key_enable(Footprint& f, uint8_t c) {
    FP_ADD(f, A_KEY_OFF + (c & 0x7fu), 1, "key off[c & 0x7f]");
    FP_ADD(f, A_KEY_OFF + c, 1, "key off[c]");
}
PORT_FN(0x00413cf0, "KeyEnable", KeyEnable_rw, fp_key_enable)
PORT_FN(0x00413d40, "KeyDisable", KeyDisable_rw, fp_key_enable)

// KeyDisableAll / KeyEnableAll (0x413d90 / 0x413db0): rep stosd over the 0x200-byte table
static void __cdecl KeyDisableAll_rw() { memset((void*)(uintptr_t)A_KEY_OFF, 1, 0x200); }
static void __cdecl KeyEnableAll_rw() { memset((void*)(uintptr_t)A_KEY_OFF, 0, 0x200); }
static void fp_key_all(Footprint& f) { FP_ADD(f, A_KEY_OFF, 0x200, "key off[]"); }
PORT_FN(0x00413d90, "KeyDisableAll", KeyDisableAll_rw, fp_key_all)
PORT_FN(0x00413db0, "KeyEnableAll", KeyEnableAll_rw, fp_key_all)

// what queue / add_to_buffer may write: the slot at the head, and the head
static void fp_key_add(Footprint& f) {
    FP_ADD(f, A_KEY_BUF + 2u * GU32(A_KEY_HEAD), 2, "key buf[head]");
    FP_ADD(f, A_KEY_HEAD, 4, "key head");
}

// KeyQueueChar (0x413dd0): the character and the key it came from, both passed on as the caller's dwords
static void __cdecl KeyQueueChar_rw(uint32_t c, uint32_t key) { queue(c, key); }
static void fp_key_queue_char(Footprint& f, uint32_t, uint32_t) { fp_key_add(f); }
PORT_FN(0x00413dd0, "KeyQueueChar", KeyQueueChar_rw, fp_key_queue_char)

// KeyQueueMetaChar (0x413df0): a character with a meta key down, queued as (meta bits | c | 0x100)
static void __cdecl KeyQueueMetaChar_rw(uint32_t c, uint32_t key) {
    if (is_meta_char(c)) queue((uint32_t)(uint16_t)(G16(A_KEY_META) | c | 0x100u), key);
}
PORT_FN(0x00413df0, "KeyQueueMetaChar", KeyQueueMetaChar_rw, fp_key_queue_char)

// KeyDown / KeyUp (0x413e20 / 0x413e70): shift 0x200, ctrl 0x800, alt 0x400
static void __cdecl KeyDown_rw(uint32_t vk) {
    if (vk == 0x10) G16(A_KEY_META) = (uint16_t)(G16(A_KEY_META) | 0x200);
    else if (vk == 0x11) G16(A_KEY_META) = (uint16_t)(G16(A_KEY_META) | 0x800);
    else if (vk == 0x12) G16(A_KEY_META) = (uint16_t)(G16(A_KEY_META) | 0x400);
}
static void __cdecl KeyUp_rw(uint32_t vk) {
    if (vk == 0x10) G16(A_KEY_META) = (uint16_t)(G16(A_KEY_META) & 0xfdff);
    else if (vk == 0x11) G16(A_KEY_META) = (uint16_t)(G16(A_KEY_META) & 0xf7ff);
    else if (vk == 0x12) G16(A_KEY_META) = (uint16_t)(G16(A_KEY_META) & 0xfbff);
}
static void fp_key_meta(Footprint& f, uint32_t) { FP_ADD(f, A_KEY_META, 2, "key meta"); }
PORT_FN(0x00413e20, "KeyDown", KeyDown_rw, fp_key_meta)
PORT_FN(0x00413e70, "KeyUp", KeyUp_rw, fp_key_meta)

// KeyClearBits (0x413ec0)
static void __cdecl KeyClearBits_rw() { G16(A_KEY_META) = 0; }
static void fp_key_clear_bits(Footprint& f) { FP_ADD(f, A_KEY_META, 2, "key meta"); }
PORT_FN(0x00413ec0, "KeyClearBits", KeyClearBits_rw, fp_key_clear_bits)

// KeyConvertScanKey (0x413ed0): MapVirtualKeyA(scan, 1), through the game's import
static char __cdecl KeyConvertScanKey_rw(uint8_t sc) {
    return (char)IAT(MapVirtualKeyA_t, IAT_MapVirtualKeyA)((uint32_t)sc, 1);
}
static void fp_key_convert(Footprint& f, uint8_t) { f.replay_only = "it calls Windows (MapVirtualKeyA)"; }
PORT_FN(0x00413ed0, "KeyConvertScanKey", KeyConvertScanKey_rw, fp_key_convert)

// queue (0x413ee0): the lock taken and dropped, then the character added unless its key is dropped
static void __cdecl queue_rw(uint32_t c, uint32_t key) {
    SingleEnter(G32(A_KEY_SINGLE), 0, 0);
    SingleLeave(G32(A_KEY_SINGLE), 0, 0);
    if (G8(A_KEY_OFF + (uint8_t)key) == 0) add_to_buffer(c);
}
PORT_FN(0x00413ee0, "queue(key.obj)", queue_rw, fp_key_queue_char)

// is_buffer_empty (0x413f30)
static uint8_t __cdecl is_buffer_empty_rw() { return GU32(A_KEY_HEAD) - GU32(A_KEY_TAIL) == 0 ? 1 : 0; }
PORT_FN(0x00413f30, "is_buffer_empty(key.obj)", is_buffer_empty_rw, fp_none)

// add_to_buffer (0x413f50): a full queue logs and drops the character
static void __cdecl add_to_buffer_rw(uint32_t c) {
    if (is_buffer_full()) {
        LogReport(k_str_key_overflow);
        return;
    }
    const uint32_t h = GU32(A_KEY_HEAD);
    G16(A_KEY_BUF + 2u * h) = (uint16_t)c;
    GU32(A_KEY_HEAD) = (uint32_t)next_element((int32_t)h);
}
static void fp_add_to_buffer(Footprint& f, uint32_t) { fp_key_add(f); }
PORT_FN(0x00413f50, "add_to_buffer(key.obj)", add_to_buffer_rw, fp_add_to_buffer)

// is_buffer_full (0x413f90)
static uint8_t __cdecl is_buffer_full_rw() {
    return (uint32_t)next_element(G32(A_KEY_HEAD)) - GU32(A_KEY_TAIL) == 0 ? 1 : 0;
}
PORT_FN(0x00413f90, "is_buffer_full(key.obj)", is_buffer_full_rw, fp_none)

// (i + 1) % n with C's sign (cdq; xor; sub; and; xor; sub), in unsigned arithmetic (no overflow at INT_MAX)
static __forceinline uint32_t mod_pow2(uint32_t v, uint32_t mask) {
    const uint32_t s = (uint32_t)((int32_t)v >> 31);
    return ((((v ^ s) - s) & mask) ^ s) - s;
}

// next_element (0x413fc0)
static int32_t __cdecl next_element_rw(int32_t i) { return (int32_t)mod_pow2((uint32_t)i + 1u, 0x1f); }
static void fp_next_element(Footprint& f, int32_t) { f.pure = true; }
PORT_FN(0x00413fc0, "next_element(key.obj)", next_element_rw, fp_next_element)

// buffer_get (0x413fe0)
static uint16_t __cdecl buffer_get_rw() {
    if (is_buffer_empty()) return 0;
    const uint32_t t = GU32(A_KEY_TAIL);
    const uint16_t v = G16(A_KEY_BUF + 2u * t);
    GU32(A_KEY_TAIL) = (uint32_t)next_element((int32_t)t);
    return v;
}
PORT_FN(0x00413fe0, "buffer_get(key.obj)", buffer_get_rw, fp_key_get)

// is_meta_char (0x414010): 0x10..0x1f but escape, 0x21..0x2f, and anything above 'Z'
static uint8_t __cdecl is_meta_char_rw(uint32_t c) {
    const uint32_t hi = c & 0xf0;
    if (hi == 0x10 && c != 0x1b) return 1;
    if (hi == 0x20 && c != 0x20) return 1;
    if (c > 0x5a) return 1;
    return 0;
}
static void fp_is_meta_char(Footprint& f, uint32_t) { f.pure = true; }
PORT_FN(0x00414010, "is_meta_char(key.obj)", is_meta_char_rw, fp_is_meta_char)

// =====================================================================================================================
// mouse.obj: the event queue (16 MouseEvents, one kept empty) and the last position / buttons
// =====================================================================================================================

// MouseBegin (0x414430)
static void __cdecl MouseBegin_rw() {
    G32(A_MOUSE_SINGLE) = SingleBegin(k_str_mouse);
    MouseClear();
}
static void fp_mouse_begin(Footprint& f) { f.replay_only = "it makes the mouse queue's lock (SingleBegin)"; }
PORT_FN(0x00414430, "MouseBegin", MouseBegin_rw, fp_mouse_begin)

// MouseEnd (0x414450)
static void __cdecl MouseEnd_rw() {
    MouseClear();
    SingleEnd(G32(A_MOUSE_SINGLE), 0, 0);
}
static void fp_mouse_end(Footprint& f) { f.replay_only = "it pumps the window's messages (MouseClear) and ends the mouse lock"; }
PORT_FN(0x00414450, "MouseEnd", MouseEnd_rw, fp_mouse_end)

// MouseClear (0x414470)
static void __cdecl MouseClear_rw() {
    Win32Idle();
    SingleEnter(G32(A_MOUSE_SINGLE), 0, 0);
    const int h = G32(A_MOUSE_SINGLE);
    G32(A_MOUSE_HEAD) = 0;
    G32(A_MOUSE_TAIL) = 0;
    SingleLeave(h, 0, 0);
}
static void fp_mouse_pump(Footprint& f) { f.replay_only = "Win32Idle pumps the window's messages"; }
PORT_FN(0x00414470, "MouseClear", MouseClear_rw, fp_mouse_pump)

// MousePeekEvent (0x4144b0): the messages pumped inside the lock
static uint8_t __cdecl MousePeekEvent_rw() {
    SingleEnter(G32(A_MOUSE_SINGLE), 0, 0);
    Win32Idle();
    const uint8_t r = mouse_is_buffer_empty() == 0 ? 1 : 0;
    SingleLeave(G32(A_MOUSE_SINGLE), 0, 0);
    return r;
}
PORT_FN(0x004144b0, "MousePeekEvent", MousePeekEvent_rw, fp_mouse_pump)

// MouseGetEvent (0x4144f0)
static uint8_t __cdecl MouseGetEvent_rw(MouseEvent* ev) {
    SingleEnter(G32(A_MOUSE_SINGLE), 0, 0);
    Win32Idle();
    const uint8_t r = mouse_buffer_get(ev);
    SingleLeave(G32(A_MOUSE_SINGLE), 0, 0);
    return r;
}
static void fp_mouse_get_event(Footprint& f, MouseEvent*) { f.replay_only = "Win32Idle pumps the window's messages"; }
PORT_FN(0x004144f0, "MouseGetEvent", MouseGetEvent_rw, fp_mouse_get_event)

// the slot mouse_buffer_add writes and the one mouse_buffer_top returns
static void fp_mouse_slots(Footprint& f) {
    const uint32_t h = GU32(A_MOUSE_HEAD);
    FP_ADD(f, A_MOUSE_BUF + 16u * h, 16, "mouse buf[head]");
    FP_ADD(f, A_MOUSE_BUF + 16u * mod_pow2(h + 0xf, 0xf), 16, "mouse buf[head - 1]");
    FP_ADD(f, A_MOUSE_HEAD, 4, "mouse head");
}

// MouseQueueEvent (0x414530): the last position and buttons kept; a move (type 6) replaces a move still at the end
// of the queue
static void __cdecl MouseQueueEvent_rw(int32_t type, int32_t x, int32_t y, int32_t state) {
    MouseEvent ev;
    SingleEnter(G32(A_MOUSE_SINGLE), 0, 0);
    ev.type = type;
    ev.x = x;
    ev.y = y;
    ev.state = state;
    G32(A_MOUSE_X) = x;
    G32(A_MOUSE_Y) = y;
    G32(A_MOUSE_STATE) = state;
    if (type == 6 && !mouse_is_buffer_empty() && mouse_buffer_top()->type == 6) {
        *mouse_buffer_top() = ev;                            // four dword moves
        SingleLeave(G32(A_MOUSE_SINGLE), 0, 0);
        return;
    }
    mouse_buffer_add(&ev);
    SingleLeave(G32(A_MOUSE_SINGLE), 0, 0);
}
static void fp_mouse_queue(Footprint& f, int32_t, int32_t, int32_t, int32_t) {
    FP_ADD(f, A_MOUSE_X, 8, "mouse x, y");
    FP_ADD(f, A_MOUSE_STATE, 4, "mouse state");
    fp_mouse_slots(f);
}
PORT_FN(0x00414530, "MouseQueueEvent", MouseQueueEvent_rw, fp_mouse_queue)

// MousePeek (0x4145f0): the last position and buttons
static void __cdecl MousePeek_rw(int32_t* x, int32_t* y, int32_t* state) {
    *x = G32(A_MOUSE_X);
    *y = G32(A_MOUSE_Y);
    *state = G32(A_MOUSE_STATE);
}
static void fp_mouse_peek(Footprint& f, int32_t* x, int32_t* y, int32_t* s) {
    f.add(x, 4, "x");
    f.add(y, 4, "y");
    f.add(s, 4, "state");
}
PORT_FN(0x004145f0, "MousePeek", MousePeek_rw, fp_mouse_peek)

// mouse_is_buffer_empty (0x414670)
static uint8_t __cdecl mouse_is_buffer_empty_rw() { return GU32(A_MOUSE_HEAD) - GU32(A_MOUSE_TAIL) == 0 ? 1 : 0; }
PORT_FN(0x00414670, "mouse_is_buffer_empty", mouse_is_buffer_empty_rw, fp_none)

// mouse_buffer_get (0x414690)
static uint8_t __cdecl mouse_buffer_get_rw(MouseEvent* ev) {
    if (mouse_is_buffer_empty()) return 0;
    const volatile int32_t* e = (const volatile int32_t*)(uintptr_t)(A_MOUSE_BUF + 16u * GU32(A_MOUSE_TAIL));
    volatile int32_t* o = (volatile int32_t*)ev;
    o[0] = e[0];
    o[1] = e[1];
    o[2] = e[2];
    o[3] = e[3];
    GU32(A_MOUSE_TAIL) = mod_pow2(GU32(A_MOUSE_TAIL) + 1u, 0xf);
    return 1;
}
static void fp_mouse_buffer_get(Footprint& f, MouseEvent* ev) {
    f.add(ev, 16, "event");
    FP_ADD(f, A_MOUSE_TAIL, 4, "mouse tail");
}
PORT_FN(0x00414690, "mouse_buffer_get", mouse_buffer_get_rw, fp_mouse_buffer_get)

// mouse_buffer_add (0x4146e0): a full queue drops the event (returns 0)
static uint8_t __cdecl mouse_buffer_add_rw(MouseEvent* ev) {
    if (mouse_is_buffer_full()) return 0;
    const volatile int32_t* e = (const volatile int32_t*)ev;
    volatile int32_t* o = (volatile int32_t*)(uintptr_t)(A_MOUSE_BUF + 16u * GU32(A_MOUSE_HEAD));
    o[0] = e[0];
    o[1] = e[1];
    o[2] = e[2];
    const uint32_t h = GU32(A_MOUSE_HEAD);                  // read between the third and fourth moves
    o[3] = e[3];
    GU32(A_MOUSE_HEAD) = mod_pow2(h + 1u, 0xf);
    return 1;
}
static void fp_mouse_buffer_add(Footprint& f, MouseEvent*) { fp_mouse_slots(f); }
PORT_FN(0x004146e0, "mouse_buffer_add", mouse_buffer_add_rw, fp_mouse_buffer_add)

// mouse_is_buffer_full (0x414730)
static uint8_t __cdecl mouse_is_buffer_full_rw() {
    return mod_pow2(GU32(A_MOUSE_HEAD) + 1u, 0xf) - GU32(A_MOUSE_TAIL) == 0 ? 1 : 0;
}
PORT_FN(0x00414730, "mouse_is_buffer_full", mouse_is_buffer_full_rw, fp_none)

// mouse_buffer_top (0x414760): the last event queued (0 when empty)
static MouseEvent* __cdecl mouse_buffer_top_rw() {
    if (mouse_is_buffer_empty()) return 0;
    return (MouseEvent*)(uintptr_t)(A_MOUSE_BUF + 16u * mod_pow2(GU32(A_MOUSE_HEAD) + 0xfu, 0xf));
}
PORT_FN(0x00414760, "mouse_buffer_top", mouse_buffer_top_rw, fp_none)

// =====================================================================================================================
// joy.obj (JoyBegin, JoyEnd, JoyGetName, JoyGetPos and the force feedback are M2's, platform.cpp)
// =====================================================================================================================

// enum_joystick (0x418c40): DirectInput's device enumeration callback (from the original JoyBegin). The first
// joystick: its name kept, a device made (data format, exclusive foreground, the DirectInput2 interface),
// autocentre turned off and a constant-force effect made on x -- and on y too unless it's the SideWinder Force
// Feedback Wheel, which fakes its y axis -- then acquired; DIENUM_STOP. Any failure: logged, DIENUM_CONTINUE.
static int __stdcall enum_joystick_rw(const uint8_t* inst, void* ref) {
    (void)ref;
    const char* name = (const char*)inst + 0x12c;               // tszProductName
    LogReport(k_fmt_joystick, name);
    rep_movs((void*)(uintptr_t)A_JOY_NAME, name, (uint32_t)strlen(name) + 1);   // the inlined strcpy
    const char* fakes[2] = {k_str_sidewinder_ffb, 0};
    uint8_t fakes_y = 0;
    for (const char* const* p = fakes; *p; p++)
        if (strcmp(*p, name) == 0) {                            // the inlined strcmp
            fakes_y = 1;
            LogReport(k_str_fakes_y);
        }
    void* dev = 0;
    void* const di = GP(void, A_JOY_DI);
    if (COM(di, 0x0c, long, const void*, void**, void*)(di, inst + 4, &dev, 0)) {   // CreateDevice(guidInstance)
        LogReport(k_str_no_device);
        return 1;
    }
    if (COM(dev, 0x2c, long, const void*)(dev, k_c_dfDIJoystick)) {                 // SetDataFormat
        LogReport(k_str_no_format);
        COM(dev, 0x08, unsigned long)(dev);                                         // Release
        return 1;
    }
    if (COM(dev, 0x34, long, void*, uint32_t)(dev, Win32GetWindow(), 5)) {           // SetCooperativeLevel
        LogReport(k_str_no_coop);
        COM(dev, 0x08, unsigned long)(dev);
        return 1;
    }
    GP(void, A_JOY_EFFECT_X) = 0;
    GP(void, A_JOY_EFFECT_Y) = 0;
    GP(void, A_JOY_DEV2) = 0;
    if (COM(dev, 0x00, long, const void*, void*)(dev, k_IID_IDirectInputDevice2, (void*)(uintptr_t)A_JOY_DEV2)) {
        LogReport(k_str_no_di2);                                                    // QueryInterface
        COM(dev, 0x08, unsigned long)(dev);
        return 1;
    }
    GP(void, A_JOY_DEV) = dev;
    if (set_prop_dword((const void*)9, 1, 0xffffffffu) == 0) LogReport(k_str_autocenter_off);   // DIPROP_AUTOCENTER
    // the locals as the original lays them out: axes, directions, the constant force and the DIEFFECT
    uint32_t axes[2], dir[2];
    int32_t force;
    uint32_t eff[13];
    memset(eff, 0, sizeof eff);
    axes[0] = 0;                                    // DIJOFS_X
    eff[0] = 0x34;                                  // dwSize
    dir[0] = 1;
    eff[1] = 0x12;                                  // dwFlags: DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS
    eff[2] = 0xffffffffu;                           // dwDuration: INFINITE
    force = 0;                                      // DICONSTANTFORCE.lMagnitude
    eff[4] = 10000;                                 // dwGain
    eff[5] = 0xffffffffu;                           // dwTriggerButton: DIEB_NOTRIGGER
    eff[7] = 1;                                     // cAxes
    eff[8] = (uint32_t)(uintptr_t)axes;             // rgdwAxes
    eff[9] = (uint32_t)(uintptr_t)dir;              // rglDirection
    eff[11] = 4;                                    // cbTypeSpecificParams
    eff[12] = (uint32_t)(uintptr_t)&force;          // lpvTypeSpecificParams
    void* dev2 = GP(void, A_JOY_DEV2);
    long hr = COM(dev2, 0x48, long, const void*, const void*, void*, void*)(dev2, k_GUID_ConstantForce, eff,
                                                                             (void*)(uintptr_t)A_JOY_EFFECT_X, 0);
    if (hr == 0) LogReport(k_str_x_effect);                                         // CreateEffect
    else if ((uint32_t)hr == E_NOTIMPL_) LogReport(k_str_x_unsupported);
    else LogReport(k_fmt_x_effect_fail, di_error(hr));
    axes[0] = 4;                                    // (a dead store in the original too)
    if (!fakes_y) {
        axes[0] = 0;                                // X and Y
        axes[1] = 4;
        dir[0] = 0;
        dir[1] = 0xffffffffu;
        eff[7] = 2;
        eff[8] = (uint32_t)(uintptr_t)axes;
        eff[9] = (uint32_t)(uintptr_t)dir;
        dev2 = GP(void, A_JOY_DEV2);
        hr = COM(dev2, 0x48, long, const void*, const void*, void*, void*)(dev2, k_GUID_ConstantForce, eff,
                                                                          (void*)(uintptr_t)A_JOY_EFFECT_Y, 0);
        if (hr != 0 && (uint32_t)hr != E_NOTIMPL_) LogReport(k_fmt_y_effect_fail, di_error(hr));
    }
    void* d = GP(void, A_JOY_DEV);
    COM(d, 0x1c, long)(d);                                                          // Acquire
    return 0;
}
static void fp_enum_joystick(Footprint& f, const uint8_t*, void*) { f.replay_only = "it makes the DirectInput joystick"; }
PORT_FN(0x00418c40, "enum_joystick", enum_joystick_rw, fp_enum_joystick)

// JoyIsPresent (0x418fc0)
static uint8_t __cdecl JoyIsPresent_rw() { return GP(void, A_JOY_DEV) ? 1 : 0; }
PORT_FN(0x00418fc0, "JoyIsPresent", JoyIsPresent_rw, fp_none)

// set_prop_dword (0x4194e0): a DIPROPDWORD on the joystick, by offset (or the whole device when obj is -1)
static int32_t __cdecl set_prop_dword_rw(const void* guid, uint32_t data, uint32_t obj) {
    uint32_t pd[5];
    pd[0] = 0x14;                                   // diph.dwSize
    pd[1] = 0x10;                                   // diph.dwHeaderSize
    pd[2] = 0;                                      // diph.dwObj
    if (obj != 0xffffffffu) pd[2] = obj;
    pd[4] = data;                                   // dwData
    pd[3] = obj + 1u == 0 ? 0u : 1u;                // diph.dwHow: DIPH_DEVICE or DIPH_BYOFFSET
    void* const dev = GP(void, A_JOY_DEV);
    return COM(dev, 0x18, long, const void*, void*)(dev, guid, pd);                 // SetProperty
}
static void fp_set_prop_dword(Footprint& f, const void*, uint32_t, uint32_t) { f.replay_only = "it sets a DirectInput property"; }
PORT_FN(0x004194e0, "set_prop_dword", set_prop_dword_rw, fp_set_prop_dword)

// =====================================================================================================================
// useful.obj
// =====================================================================================================================

// UsefulBegin (0x419560): the random numbers, 10000 nodes, the resources
static void __cdecl UsefulBegin_rw() {
    RandomBegin();
    NodeBegin(10000);
    ResourceBegin(k_str_dot2, k_str_dot1);
}
static void fp_useful_begin(Footprint& f) { f.replay_only = "it begins the random numbers, the node pool and the resources"; }
PORT_FN(0x00419560, "UsefulBegin", UsefulBegin_rw, fp_useful_begin)

// UsefulEnd (0x419590)
static void __cdecl UsefulEnd_rw() {
    ResourceEnd();
    NodeEnd();
    RandomEnd();
}
static void fp_useful_end(Footprint& f) { f.replay_only = "it ends the resources, the node pool and the random numbers"; }
PORT_FN(0x00419590, "UsefulEnd", UsefulEnd_rw, fp_useful_end)

// =====================================================================================================================
// base64.obj: 6 bits a character, the alphabet a-z A-Z 0-9 # $ (0x4eb400), no padding
// =====================================================================================================================

// what from_64 may write: its table, built on first use, and the flag
static void fp_from64_statics(Footprint& f) {
    FP_ADD(f, A_B64_TABLE, 0x100, "base64 table");
    FP_ADD(f, A_B64_INIT, 1, "base64 table to build");
}

// Base64ToMem (0x41b020): up to max bytes from a string whose length is a multiple of 4; 0 on a bad character
// (after the bytes before it are written) or a bad length
static uint8_t __cdecl Base64ToMem_rw(uint8_t* dst, int32_t max, const char* src) {
    uint8_t ok = 1;
    const int32_t len = (int32_t)strlen(src);
    if (len & 3) return 0;
    int32_t in = 0, out = 0;
    if (len > 0) {
        do {
            if (out >= max) break;
            const uint32_t t = base64_to_triple(src + in);
            if (t & 0xff000000u) { ok = 0; break; }
            for (int32_t sh = 16; out < max && sh >= 0; sh -= 8) {
                out++;
                dst[out - 1] = (uint8_t)(t >> sh);
            }
            in += 4;
        } while (in < len);
    }
    return ok;
}
static void fp_base64_to_mem(Footprint& f, uint8_t* dst, int32_t max, const char* src) {
    const int32_t len = (int32_t)strlen(src);
    const int32_t n = (len & 3) || max <= 0 ? 0 : (len / 4 * 3 < max ? len / 4 * 3 : max);
    if (n > 0) f.add(dst, (uint32_t)n, "bytes");
    fp_from64_statics(f);
}
PORT_FN(0x0041b020, "Base64ToMem", Base64ToMem_rw, fp_base64_to_mem)

// base64_to_triple (0x41b0c0): four characters to 24 bits; 0xff000000 on a bad one
static uint32_t __cdecl base64_to_triple_rw(const char* s) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v <<= 6;
        const uint32_t c = from_64((uint32_t)(uint8_t)s[i]);
        if ((uint8_t)c & 0xc0) return 0xff000000u;
        v |= (uint8_t)c;
    }
    return v;
}
static void fp_base64_to_triple(Footprint& f, const char*) { fp_from64_statics(f); }
PORT_FN(0x0041b0c0, "base64_to_triple", base64_to_triple_rw, fp_base64_to_triple)

// from_64 (0x41b100): a character's value (0xff if not in the alphabet); the table is built on first use. The
// character is sign-extended, so bytes >= 0x80 read the 128 bytes before the table.
static uint8_t __cdecl from_64_rw(uint32_t c) {
    if (G8(A_B64_INIT)) {
        G8(A_B64_INIT) = 0;
        memset((void*)(uintptr_t)A_B64_TABLE, 0xff, 0x100);
        for (int32_t i = 0; i < 0x40; i++)
            G8(A_B64_TABLE + (int32_t)(int8_t)G8(A_B64_ALPHABET + i)) = (uint8_t)i;
    }
    return G8(A_B64_TABLE + (int32_t)(int8_t)(uint8_t)c);
}
static void fp_from_64(Footprint& f, uint32_t) { fp_from64_statics(f); }
PORT_FN(0x0041b100, "from_64", from_64_rw, fp_from_64)

// MemToBase64 (0x41b150): n bytes, 3 at a time (zeros past the end), 4 characters each while fewer than max have
// been written, then a terminator -- so up to 4 characters and the terminator past max
static uint8_t __cdecl MemToBase64_rw(char* dst, int32_t max, const uint8_t* src, int32_t n) {
    int32_t in = 0, out = 0;
    if (n > 0) {
        do {
            if (out >= max) break;
            uint32_t t = 0;
            for (int k = 3; k; k--) {
                t <<= 8;
                if (in < n) t |= src[in];
                in++;
            }
            triple_to_base64(dst + out, t);
            out += 4;
        } while (in < n);
    }
    dst[out] = 0;
    return 1;
}
static void fp_mem_to_base64(Footprint& f, char* dst, int32_t max, const uint8_t*, int32_t n) {
    int32_t g = 0;
    if (n > 0 && max > 0) {
        const int32_t a = (n + 2) / 3, b = (max + 3) / 4;
        g = a < b ? a : b;
    }
    f.add(dst, (uint32_t)(4 * g + 1), "characters");
    f.pure = true;
}
PORT_FN(0x0041b150, "MemToBase64", MemToBase64_rw, fp_mem_to_base64)

// triple_to_base64 (0x41b1b0): 24 bits to four characters, last first
static void __cdecl triple_to_base64_rw(char* d, uint32_t t) {
    for (int32_t i = 3; i >= 0; i--) {
        d[i] = to_64(t & 0x3f);
        t >>= 6;
    }
}
static void fp_triple_to_base64(Footprint& f, char* d, uint32_t) {
    f.add(d, 4, "characters");
    f.pure = true;
}
PORT_FN(0x0041b1b0, "triple_to_base64", triple_to_base64_rw, fp_triple_to_base64)

// to_64 (0x41b1e0): the alphabet's character (any byte indexes it: 64..255 read what follows the alphabet)
static char __cdecl to_64_rw(uint8_t v) { return (char)G8(A_B64_ALPHABET + v); }
static void fp_to_64(Footprint& f, uint8_t) { f.pure = true; }
PORT_FN(0x0041b1e0, "to_64", to_64_rw, fp_to_64)

// =====================================================================================================================
// table.obj: string tables (STAB resources): rows x columns of strings, each row rowsize bytes at +0x4c, a column
// at its offset in the row
// =====================================================================================================================

// StringTableGet (0x41b210): the resource; a missing table or a non-zero version is a panic
static StringTable* __cdecl StringTableGet_rw(const char* name) {
    uint32_t version;
    StringTable* t = (StringTable*)ResourceGet(name, 0x53544142, &version, 0, 0, 0);   // 'STAB'
    if (!t) LogPanic(k_fmt_no_table, name);
    if (version != 0) LogPanic(k_fmt_table_version, version, 0, name);
    return t;
}
static void fp_st_get(Footprint& f, const char*) { f.replay_only = "it loads a resource"; }
PORT_FN(0x0041b210, "StringTableGet", StringTableGet_rw, fp_st_get)

// StringTableForget (0x41b270)
static void __cdecl StringTableForget_rw(StringTable* t) { ResourceForget(t); }
static void fp_st_forget(Footprint& f, StringTable*) { f.replay_only = "it releases a resource"; }
PORT_FN(0x0041b270, "StringTableForget", StringTableForget_rw, fp_st_forget)

// StringTableNumColumns / StringTableNumRows (0x41b280 / 0x41b290)
static int32_t __cdecl StringTableNumColumns_rw(const StringTable* t) { return t->cols; }
static int32_t __cdecl StringTableNumRows_rw(const StringTable* t) { return t->rows; }
static void fp_st_pure(Footprint& f, const StringTable*) { f.pure = true; }
PORT_FN(0x0041b280, "StringTableNumColumns", StringTableNumColumns_rw, fp_st_pure)
PORT_FN(0x0041b290, "StringTableNumRows", StringTableNumRows_rw, fp_st_pure)

// StringTableGetEntry (0x41b2a0)
static const char* __cdecl StringTableGetEntry_rw(const StringTable* t, int32_t row, int32_t col) {
    return (const char*)t + (uint32_t)t->coloff[col] + (uint32_t)t->rowsize * (uint32_t)row + 0x4c;
}
static void fp_st_entry(Footprint&, const StringTable*, int32_t, int32_t) {}
PORT_FN(0x0041b2a0, "StringTableGetEntry", StringTableGetEntry_rw, fp_st_entry)

// StringTableSort (0x41b2c0): passes of neighbour compares from each row on; where row a sorts before row b
// (stricmp < 0) the rows are "swapped" through a 0x400-byte buffer -- but the copies are saved = a, a = b, a = saved,
// so no row ever changes (and a row longer than 0x400 bytes overruns the stack)
static void __cdecl StringTableSort_rw(StringTable* t, int32_t col) {
    uint8_t saved[0x400];
    int32_t i = 0;
    if (t->rows <= 0) return;
    do {
        int32_t a = i;
        i = i + 1;
        if (t->rows > i) {
            int32_t b;
            do {
                b = a + 1;
                const char* eb = StringTableGetEntry(t, b, col);
                const char* ea = StringTableGetEntry(t, a, col);
                if (game_stricmp(ea, eb) < 0) {
                    uint32_t rs = (uint32_t)t->rowsize;
                    rep_movs(saved, (uint8_t*)t + rs * (uint32_t)a + 0x4c, rs);
                    rs = (uint32_t)t->rowsize;
                    rep_movs((uint8_t*)t + rs * (uint32_t)a + 0x4c, (uint8_t*)t + rs * (uint32_t)b + 0x4c, rs);
                    rs = (uint32_t)t->rowsize;
                    rep_movs((uint8_t*)t + rs * (uint32_t)a + 0x4c, saved, rs);
                }
                a = b;
            } while (b + 1 < t->rows);
        }
    } while (t->rows > i);
}
static void fp_st_sort(Footprint& f, StringTable* t, int32_t) {
    if (t->rows > 0 && t->rowsize > 0) f.add(t->data, (uint32_t)t->rows * (uint32_t)t->rowsize, "rows");
}
PORT_FN(0x0041b2c0, "StringTableSort", StringTableSort_rw, fp_st_sort)

// StringTableFind (0x41b3b0): a binary search that stops when its step reaches 0 (row 0 is never compared)
static uint8_t __cdecl StringTableFind_rw(StringTable* t, int32_t col, const char* key, int32_t* row) {
    const int32_t rows = t->rows;
    int32_t lo = 0, hi = rows - 1;
    int32_t mid = rows / 2, step = rows / 2;
    while (step > 0) {
        const int r = game_stricmp(StringTableGetEntry(t, mid, col), key);
        if (r == 0) break;
        if (r > 0) lo = mid;
        else hi = mid;
        step = (int32_t)((uint32_t)hi - (uint32_t)lo) / 2;
        mid = (int32_t)((uint32_t)step + (uint32_t)lo);
    }
    if (step > 0) {
        *row = mid;
        return 1;
    }
    return 0;
}
static void fp_st_find(Footprint& f, StringTable*, int32_t, const char*, int32_t* row) { f.add(row, 4, "row"); }
PORT_FN(0x0041b3b0, "StringTableFind", StringTableFind_rw, fp_st_find)

// =====================================================================================================================
// node.obj: a pool of 16-byte nodes, with a count of the ones reserved
// =====================================================================================================================

// NodeBegin (0x41b460)
static void __cdecl NodeBegin_rw(int32_t n) {
    G32(A_NODE_MULTI) = MultiBegin(k_str_node);
    G32(A_NODE_GRABBED) = 0;
    G32(A_NODE_MAX) = n;
    PoolBase* p = (PoolBase*)MemAlloc(0x20);
    if (p) {
        PoolBase_ctor(p, 0, k_str_nodes, G32(A_NODE_MAX), 0x10);
        p->vtbl = (void*)k_vt_PoolNode;
        GP(PoolBase, A_NODE_POOL) = p;
    } else {
        GP(PoolBase, A_NODE_POOL) = 0;
    }
}
static void fp_node_begin(Footprint& f, int32_t) { f.replay_only = "it makes the node lock and allocates the pool"; }
PORT_FN(0x0041b460, "NodeBegin", NodeBegin_rw, fp_node_begin)

// NodeEnd (0x41b4d0): the pool deleted through its vtable (scalar deleting destructor, 1)
static void __cdecl NodeEnd_rw() {
    PoolBase* p = GP(PoolBase, A_NODE_POOL);
    if (p) ((void*(__fastcall*)(void*, Edx, uint32_t))(((void**)p->vtbl)[0]))(p, 0, 1);
    const int h = G32(A_NODE_MULTI);
    GP(PoolBase, A_NODE_POOL) = 0;
    G32(A_NODE_MAX) = 0;
    G32(A_NODE_GRABBED) = 0;
    MultiEnd(h, 0, 0);
}
static void fp_node_end(Footprint& f) { f.replay_only = "it deletes the pool and ends the node lock"; }
PORT_FN(0x0041b4d0, "NodeEnd", NodeEnd_rw, fp_node_end)

// NodeGrab (0x41b510): n more reserved, unless that reaches the pool's size
static void __cdecl NodeGrab_rw(int32_t n) {
    MultiEnter(G32(A_NODE_MULTI), 0, 0);
    const int32_t c = (int32_t)((uint32_t)n + GU32(A_NODE_GRABBED));
    if (G32(A_NODE_MAX) > c) G32(A_NODE_GRABBED) = c;
    else LogReport(k_str_node_reserved);
    MultiLeave(G32(A_NODE_MULTI), 0, 0);
}
static void fp_node_grab(Footprint& f, int32_t) { FP_ADD(f, A_NODE_GRABBED, 4, "nodes reserved"); }
PORT_FN(0x0041b510, "NodeGrab", NodeGrab_rw, fp_node_grab)

// NodeRelease (0x41b560): n fewer reserved, unless the count's sign would go negative (js)
static void __cdecl NodeRelease_rw(int32_t n) {
    MultiEnter(G32(A_NODE_MULTI), 0, 0);
    const uint32_t c = GU32(A_NODE_GRABBED) - (uint32_t)n;
    if ((int32_t)c >= 0) GU32(A_NODE_GRABBED) = c;
    else LogReport(k_str_node_released);
    MultiLeave(G32(A_NODE_MULTI), 0, 0);
}
PORT_FN(0x0041b560, "NodeRelease", NodeRelease_rw, fp_node_grab)

// NodeAlloc / NodeFree (0x41b5b0 / 0x41b5f0): the pool under the node lock
static void* __cdecl NodeAlloc_rw() {
    MultiEnter(G32(A_NODE_MULTI), 0, 0);
    void* r = PoolBase_alloc(GP(PoolBase, A_NODE_POOL), 0);
    MultiLeave(G32(A_NODE_MULTI), 0, 0);
    return r;
}
static void fp_node_alloc(Footprint& f) {
    if (PoolBase* p = GP(PoolBase, A_NODE_POOL)) f.add(p, 0x20, "node pool");
}
PORT_FN(0x0041b5b0, "NodeAlloc", NodeAlloc_rw, fp_node_alloc)
static void __cdecl NodeFree_rw(void* node) {
    MultiEnter(G32(A_NODE_MULTI), 0, 0);
    PoolBase_free(GP(PoolBase, A_NODE_POOL), 0, node);
    MultiLeave(G32(A_NODE_MULTI), 0, 0);
}
static void fp_node_free(Footprint& f, void* node) {
    fp_node_alloc(f);
    f.add(node, 4, "node's link");
}
PORT_FN(0x0041b5f0, "NodeFree", NodeFree_rw, fp_node_free)

// =====================================================================================================================
// random.obj: Marsaglia's universal generator (RANMAR), in floats. rmarin seeds the 97-entry table, ranmar draws.
// The seeding runs on the x87 through the C runtime's _CIfmod; its steps are exact integers except the first two
// products (ij / 177, kl / 169 as a multiply by a double reciprocal), which round as the FPU's precision says.
// =====================================================================================================================

// (u[i97] is the entry a draw writes: listed separately in case the index has gone out of range)
static void fp_ran_state(Footprint& f) {
#if !defined(VP_FAITHFUL) && !defined(VP_FUZZ)
    // Both threads draw. A check on the main thread would save and restore the state outside the generator's
    // lock, losing or repeating a physics-thread draw made in between: those are checked on the physics thread.
    if (::GetCurrentThreadId() != g_physics_thread_id) { f.replay_only = "a main-thread draw (the physics thread shares the state)"; return; }
#endif
    FP_ADD(f, A_RAN_FIRST, RAN_BYTES, "random state (c, u[97], cd, cm, i97, j97)");
    FP_ADD(f, A_RAN_U + 4u * GU32(A_RAN_I97), 4, "random u[i97]");
}

// RandomBegin (0x41b670)
static void __cdecl RandomBegin_rw() {
    G32(A_RAN_MULTI) = MultiBegin(k_str_random);
    Randomize();
}
static void fp_random_begin(Footprint& f) { f.replay_only = "it makes the random lock and seeds from the clock"; }
PORT_FN(0x0041b670, "RandomBegin", RandomBegin_rw, fp_random_begin)

// RandomEnd (0x41b690)
static void __cdecl RandomEnd_rw() { MultiEnd(G32(A_RAN_MULTI), 0, 0); }
static void fp_random_end(Footprint& f) { f.replay_only = "it ends the random lock"; }
PORT_FN(0x0041b690, "RandomEnd", RandomEnd_rw, fp_random_end)

// Randomize (0x41b6b0): seeded from the clock mod 30000, twice -- the first reading is the second seed
static void __cdecl Randomize_rw() {
    const int32_t kl = PTimeNow() % 30000;
    const int32_t ij = PTimeNow() % 30000;
    rmarin(ij, kl);
}
static void fp_randomize(Footprint& f) { f.replay_only = "it seeds from the clock"; }
PORT_FN(0x0041b6b0, "Randomize", Randomize_rw, fp_randomize)

// Random (0x41b6e0): ranmar() * n, chopped (__ftol)
static int32_t __cdecl Random1_rw(int32_t n) { return x87_ftol((double)ranmar() * n); }
static void fp_random1(Footprint& f, int32_t) { fp_ran_state(f); }
PORT_FN(0x0041b6e0, "Random(int)", Random1_rw, fp_random1)

// Random (0x41b700): lo + Random(hi - lo), through the one above (the recorder's hook, in the game). The state it
// draws from is the inner Random's input, so it isn't this function's footprint (a shadow check feeds the rewrite
// the inner call's value).
static int32_t __cdecl Random2_rw(int32_t lo, int32_t hi) {
    return (int32_t)((uint32_t)Random((int32_t)((uint32_t)hi - (uint32_t)lo)) + (uint32_t)lo);
}
static void fp_random2(Footprint&, int32_t, int32_t) {}
PORT_FN(0x0041b700, "Random(int,int)", Random2_rw, fp_random2)

// fild x; fmul qword k; fld qword y; call _CIfmod -- the remainder is exact, so a double holds it
VP_ASM_CALLS static double ran_fmod_scaled(int32_t x, double k, double y) {
    double r;
    __asm { fild x
            fmul k
            fld y
            mov eax, k_CIfmod
            call eax
            fstp r }
    return r;
}
// fild x; fld qword y; call _CIfmod
VP_ASM_CALLS static double ran_fmod_int(int32_t x, double y) {
    double r;
    __asm { fild x
            fld y
            mov eax, k_CIfmod
            call eax
            fstp r }
    return r;
}
// (an exact remainder) fimul k; fld qword y; call _CIfmod
VP_ASM_CALLS static double ran_fmod_mul(double v, int32_t k, double y) {
    double r;
    __asm { fld v
            fimul k
            fld y
            mov eax, k_CIfmod
            call eax
            fstp r }
    return r;
}
// fild l; fmul qword 53.0; fadd 1.0 (the register the original keeps); fld qword 169.0; call _CIfmod
VP_ASM_CALLS static double ran_fmod_lcg(int32_t l, double k53, double one, double y) {
    double r;
    __asm { fild l
            fmul k53
            fadd one
            fld y
            mov eax, k_CIfmod
            call eax
            fstp r }
    return r;
}

// rmarin (0x41b720): the table seeded from ij and kl (Marsaglia's seeds 0 <= ij <= 31328, 0 <= kl <= 30081)
static void __cdecl rmarin_rw(int32_t ij, int32_t kl) {
    MultiEnter(G32(A_RAN_MULTI), 0, 0);
    const double inv177 = Db(0x3f7724287f46debcull), inv169 = Db(0x3f783c977ab2bedd);   // 1/177, 1/169
    int32_t i = x87_ftol(ran_fmod_scaled(ij, inv177, 177.0)) + 2;
    int32_t j = x87_ftol(ran_fmod_int(ij, 177.0)) + 2;
    int32_t k = x87_ftol(ran_fmod_scaled(kl, inv169, 178.0)) + 1;
    int32_t l = x87_ftol(ran_fmod_int(kl, 169.0));
    for (uint32_t p = A_RAN_U; p <= A_RAN_U + 96 * 4; p += 4) {
        double s = 0.0f;                                     // fld dword 0.0
        float t = 0.5f;
        for (int32_t n = 24; n; n--) {
            const double r = ran_fmod_int((int32_t)((uint32_t)j * (uint32_t)i), 179.0);
            const int32_t m = x87_ftol(ran_fmod_mul(r, k, 179.0));
            i = j;
            j = k;
            k = m;
            l = x87_ftol(ran_fmod_lcg(l, 53.0, 1.0, 169.0));
            if (!(ran_fmod_int((int32_t)((uint32_t)l * (uint32_t)m), 64.0) < 32.0)) s = s + t;   // fcomp; test ah, 1
            t = (float)(D(t) * 0.5);
        }
        GF(p) = (float)s;
    }
    GU32(A_RAN_C) = 0x3cb0f880u;                             // 362436 / 16777216
    GU32(A_RAN_CD) = 0x3ee99762u;                            // 7654321 / 16777216
    GU32(A_RAN_CM) = 0x3f7ffffdu;                            // 16777213 / 16777216
    G32(A_RAN_I97) = 0x60;
    G32(A_RAN_J97) = 0x20;
    G8(A_RAN_TEST) = 1;
    MultiLeave(G32(A_RAN_MULTI), 0, 0);
}
static void fp_rmarin(Footprint& f, int32_t, int32_t) {
    fp_ran_state(f);
    FP_ADD(f, A_RAN_TEST, 1, "random seeded");
}
PORT_FN(0x0041b720, "rmarin", rmarin_rw, fp_rmarin)

// ranmar (0x41b910): one draw in [0, 1)
static float __cdecl ranmar_rw() {
    MultiEnter(G32(A_RAN_MULTI), 0, 0);
    const int32_t i = G32(A_RAN_I97), j = G32(A_RAN_J97);
    const uint32_t ui = A_RAN_U + 4u * (uint32_t)i;
    const double d = D(GF(ui)) - GF(A_RAN_U + 4u * (uint32_t)j);
    float uni = (float)d;
    if (!(d >= 0.0f)) uni = (float)(D(uni) + 1.0f);          // fcom 0.0f; test ah, 1
    GU32(ui) = Ub(uni);                                      // mov: a bit copy
    const int32_t ni = (int32_t)((uint32_t)i - 1u);
    G32(A_RAN_I97) = ni < 0 ? 0x60 : ni;
    const int32_t nj = (int32_t)((uint32_t)j - 1u);
    G32(A_RAN_J97) = nj < 0 ? 0x60 : nj;
    const double dc = D(GF(A_RAN_C)) - GF(A_RAN_CD);
    GF(A_RAN_C) = (float)dc;
    if (!(dc >= 0.0f)) GF(A_RAN_C) = (float)(D(GF(A_RAN_CM)) + GF(A_RAN_C));
    const double du = D(uni) - GF(A_RAN_C);
    uni = (float)du;
    if (!(du >= 0.0f)) uni = (float)(D(uni) + 1.0f);
    MultiLeave(G32(A_RAN_MULTI), 0, 0);
    return uni;
}
static void fp_ranmar(Footprint& f) { fp_ran_state(f); }
PORT_FN(0x0041b910, "ranmar", ranmar_rw, fp_ranmar)

// =====================================================================================================================
// pool.obj: PoolBase (a block of count elements of size bytes, a free list through their first dwords) and
// DebugPoolBase (each element separately allocated, tracked by a list of blocks from a PoolBase)
// =====================================================================================================================

// PoolBase::PoolBase (0x41ba30)
static PoolBase* __fastcall PoolBase_ctor_rw(PoolBase* self, Edx, const char* name, int32_t count, uint32_t size) {
    self->vtbl = (void*)k_vt_PoolBase;
    self->size = size;
    self->name = name;
    self->crashes = 1;
    self->count = count;
    uint8_t* m = (uint8_t*)MemAlloc((int)((uint32_t)count * size));
    self->mem = m;
    if (m) {
        self->head = m;
        PoolBase_init_list(self, 0);
        return self;
    }
    LogPanic(k_str_pool_nomem);
    return self;
}
static void fp_pool_ctor(Footprint& f, PoolBase*, Edx, const char*, int32_t, uint32_t) { f.replay_only = "it allocates the pool's block"; }
PORT_FN(0x0041ba30, "PoolBase::PoolBase", PoolBase_ctor_rw, fp_pool_ctor)

// what init_list writes: the links from the head, the last one's 0, and the count used
static void fp_pool_list(Footprint& f, PoolBase* self) {
    const uint32_t head = (uint32_t)(uintptr_t)self->head;
    const uint32_t last = head + ((uint32_t)self->count - 1u) * self->size;
    if (head < last) FP_ADD(f, head, last - head + 4, "pool elements' links");
    else FP_ADD(f, last, 4, "pool's last link");
    f.add(self, 0x20, "pool");
}

// PoolBase::init_list (0x41ba90): every element linked to the next from the head, the last to 0
static void __fastcall PoolBase_init_list_rw(PoolBase* self, Edx) {
    volatile uint32_t* const size = &self->size;
    uint8_t* p = (uint8_t*)self->head;
    uint8_t* const end = p + ((uint32_t)self->count - 1u) * self->size;
    while (p < end) {
        *(uint8_t* volatile*)p = p + *size;
        p += *size;
    }
    *(void* volatile*)end = 0;
    self->used = 0;
}
static void fp_pool_init_list(Footprint& f, PoolBase* self, Edx) { fp_pool_list(f, self); }
PORT_FN(0x0041ba90, "PoolBase::init_list", PoolBase_init_list_rw, fp_pool_init_list)

// PoolBase::~PoolBase (0x41bac0)
static void __fastcall PoolBase_dtor_rw(PoolBase* self, Edx) {
    const char* name = self->name;
    self->vtbl = (void*)k_vt_PoolBase;
    ASSERT_MSG_pool((uint32_t)self->used < 1u ? 1 : 0, k_fmt_pool_destroyed, name);
    MemFree(self->mem);
    self->mem = 0;
}
static void fp_pool_dtor(Footprint& f, PoolBase*, Edx) { f.replay_only = "it frees the pool's block"; }
PORT_FN(0x0041bac0, "PoolBase::~PoolBase", PoolBase_dtor_rw, fp_pool_dtor)

// ASSERT_MSG (pool.obj 0x41bb00): compiled out -- a bare ret
static void __cdecl ASSERT_MSG_pool_rw(int, const char*) {}
static void fp_assert_msg(Footprint& f, int, const char*) { f.pure = true; }
PORT_FN(0x0041bb00, "ASSERT_MSG(pool.obj)", ASSERT_MSG_pool_rw, fp_assert_msg)

// PoolBase::OverallocCrashes (0x41bb10)
static void __fastcall PoolBase_OverallocCrashes_rw(PoolBase* self, Edx, uint8_t b) { self->crashes = b; }
static void fp_pool_overalloc(Footprint& f, PoolBase* self, Edx, uint8_t) { f.add(self, 0x20, "pool"); }
PORT_FN(0x0041bb10, "PoolBase::OverallocCrashes", PoolBase_OverallocCrashes_rw, fp_pool_overalloc)

// PoolBase::alloc (0x41bb20): the head of the free list; empty: 0, or a panic when overallocating crashes
static void* __fastcall PoolBase_alloc_rw(PoolBase* self, Edx) {
    void* const r = self->head;
    if (!r) {
        if (!self->crashes) return 0;
        LogPanic(k_fmt_pool_oom, self->name);
    }
    if (void** h = (void**)self->head) self->head = *h;
    self->used++;
    return r;
}
static void fp_pool_alloc(Footprint& f, PoolBase* self, Edx) { f.add(self, 0x20, "pool"); }
PORT_FN(0x0041bb20, "PoolBase::alloc", PoolBase_alloc_rw, fp_pool_alloc)

// PoolBase::free (0x41bb60)
static void __fastcall PoolBase_free_rw(PoolBase* self, Edx, void* p) {
    void* const h = self->head;
    *(void* volatile*)p = h;
    self->head = p;
    self->used--;
}
static void fp_pool_free(Footprint& f, PoolBase* self, Edx, void* p) {
    f.add(p, 4, "element's link");
    f.add(self, 0x20, "pool");
}
PORT_FN(0x0041bb60, "PoolBase::free", PoolBase_free_rw, fp_pool_free)

// PoolBase::freeall (0x41bb80)
static void __fastcall PoolBase_freeall_rw(PoolBase* self, Edx) {
    self->head = self->mem;
    PoolBase_init_list(self, 0);
}
static void fp_pool_freeall(Footprint& f, PoolBase* self, Edx) {
    PoolBase p = *self;
    p.head = p.mem;
    fp_pool_list(f, &p);
    f.add(self, 0x20, "pool");
}
PORT_FN(0x0041bb80, "PoolBase::freeall", PoolBase_freeall_rw, fp_pool_freeall)

// DebugPoolBase::DebugPoolBase (0x41bb90)
static DebugPoolBase* __fastcall DebugPoolBase_ctor_rw(DebugPoolBase* self, Edx, const char* name, int32_t count,
                                                       uint32_t size) {
    PoolBase_ctor(&self->pool, 0, k_str_debugpool_pool, count, 8);
    self->pool.vtbl = (void*)k_vt_PoolBlock;
    self->vtbl = (void*)k_vt_DebugPoolBase;
    self->name = name;
    self->max = count;
    self->blocks = 0;
    self->used = 0;
    self->size = size;
    self->crashes = 1;
    return self;
}
static void fp_dpool_ctor(Footprint& f, DebugPoolBase*, Edx, const char*, int32_t, uint32_t) { f.replay_only = "it allocates its pool's block"; }
PORT_FN(0x0041bb90, "DebugPoolBase::DebugPoolBase", DebugPoolBase_ctor_rw, fp_dpool_ctor)

// DebugPoolBase::~DebugPoolBase (0x41bbe0): the elements still allocated aren't freed
static void __fastcall DebugPoolBase_dtor_rw(DebugPoolBase* self, Edx) {
    const char* name = self->name;
    self->vtbl = (void*)k_vt_DebugPoolBase;
    ASSERT_MSG_pool((uint32_t)self->used < 1u ? 1 : 0, k_fmt_dpool_destroyed, name);
    self->name = 0;
    self->max = 0;
    self->size = 0;
    PoolBase_dtor(&self->pool, 0);
}
static void fp_dpool_dtor(Footprint& f, DebugPoolBase*, Edx) { f.replay_only = "it frees its pool's block"; }
PORT_FN(0x0041bbe0, "DebugPoolBase::~DebugPoolBase", DebugPoolBase_dtor_rw, fp_dpool_dtor)

// DebugPoolBase::OverallocCrashes (0x41bc20)
static void __fastcall DebugPoolBase_OverallocCrashes_rw(DebugPoolBase* self, Edx, uint8_t b) { self->crashes = b; }
static void fp_dpool_overalloc(Footprint& f, DebugPoolBase* self, Edx, uint8_t) { f.add(self, 0x3c, "debug pool"); }
PORT_FN(0x0041bc20, "DebugPoolBase::OverallocCrashes", DebugPoolBase_OverallocCrashes_rw, fp_dpool_overalloc)

// DebugPoolBase::alloc (0x41bc30): past max, 0 -- or, when overallocating crashes, an assertion (a no-op) and on
static void* __fastcall DebugPoolBase_alloc_rw(DebugPoolBase* self, Edx) {
    if (self->crashes) {
        ASSERT_MSG_pool(self->max > self->used ? 1 : 0, k_fmt_overalloc, self->name);
    } else if (!(self->max > self->used)) {
        return 0;
    }
    self->used++;
    DebugBlock* b = (DebugBlock*)PoolBase_alloc(&self->pool, 0);
    b->next = self->blocks;
    self->blocks = b;
    b->mem = MemAlloc((int)self->size);
    if (!b->mem) LogPanic(k_fmt_dpool_oom, self->name);
    return b->mem;
}
static void fp_dpool_alloc(Footprint& f, DebugPoolBase*, Edx) { f.replay_only = "it allocates the element"; }
PORT_FN(0x0041bc30, "DebugPoolBase::alloc", DebugPoolBase_alloc_rw, fp_dpool_alloc)

// DebugPoolBase::free (0x41bcb0): the element's block found in the list (else a panic), unlinked, freed
static void __fastcall DebugPoolBase_free_rw(DebugPoolBase* self, Edx, void* p) {
    DebugBlock* b = self->blocks;
    DebugBlock* prev = 0;
    for (; b; prev = b, b = b->next)
        if (b->mem == p) break;
    if (!b) {
        LogPanic(k_fmt_dpool_bad_free, self->name);
        return;
    }
    if (prev) prev->next = b->next;
    else self->blocks = b->next;
    MemFree(b->mem);
    PoolBase_free(&self->pool, 0, b);
    self->used--;
}
static void fp_dpool_free(Footprint& f, DebugPoolBase*, Edx, void*) { f.replay_only = "it frees the element"; }
PORT_FN(0x0041bcb0, "DebugPoolBase::free", DebugPoolBase_free_rw, fp_dpool_free)

// DebugPoolBase::freeall (0x41bd10)
static void __fastcall DebugPoolBase_freeall_rw(DebugPoolBase* self, Edx) {
    for (DebugBlock* b = self->blocks; b;) {
        DebugBlock* const next = b->next;
        MemFree(b->mem);
        b = next;
    }
    PoolBase_freeall(&self->pool, 0);
    self->blocks = 0;
    self->used = 0;
}
static void fp_dpool_freeall(Footprint& f, DebugPoolBase*, Edx) { f.replay_only = "it frees every element"; }
PORT_FN(0x0041bd10, "DebugPoolBase::freeall", DebugPoolBase_freeall_rw, fp_dpool_freeall)

// DebugPoolBase::is_member (0x41bd50)
static uint8_t __fastcall DebugPoolBase_is_member_rw(DebugPoolBase* self, Edx, void* p) {
    for (DebugBlock* b = self->blocks; b; b = b->next)
        if (b->mem == p) return 1;
    return 0;
}
static void fp_dpool_is_member(Footprint&, DebugPoolBase*, Edx, void*) {}
PORT_FN(0x0041bd50, "DebugPoolBase::is_member", DebugPoolBase_is_member_rw, fp_dpool_is_member)

// the scalar deleting destructors (0x41b630 Pool<Node>, 0x41bd70 PoolBase, 0x41bd90 Pool<block>, 0x41bdb0
// DebugPoolBase): the destructor, then operator delete when bit 0 of the flags is set
static void* __fastcall PoolNode_sdd_rw(PoolBase* self, Edx, uint32_t flags) {
    PoolBase_dtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
static void* __fastcall PoolBase_sdd_rw(PoolBase* self, Edx, uint32_t flags) {
    PoolBase_dtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
static void* __fastcall PoolBlock_sdd_rw(PoolBase* self, Edx, uint32_t flags) {
    PoolBase_dtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
static void* __fastcall DebugPoolBase_sdd_rw(DebugPoolBase* self, Edx, uint32_t flags) {
    DebugPoolBase_dtor(self, 0);
    if (flags & 1) OperatorDelete(self);
    return self;
}
static void fp_pool_sdd(Footprint& f, PoolBase*, Edx, uint32_t) { f.replay_only = "it frees the pool"; }
static void fp_dpool_sdd(Footprint& f, DebugPoolBase*, Edx, uint32_t) { f.replay_only = "it frees the pool"; }
PORT_FN(0x0041b630, "Pool<Node>::scalar deleting destructor", PoolNode_sdd_rw, fp_pool_sdd)
PORT_FN(0x0041bd70, "PoolBase::scalar deleting destructor", PoolBase_sdd_rw, fp_pool_sdd)
PORT_FN(0x0041bd90, "Pool<DebugPoolBase::block>::scalar deleting destructor", PoolBlock_sdd_rw, fp_pool_sdd)
PORT_FN(0x0041bdb0, "DebugPoolBase::scalar deleting destructor", DebugPoolBase_sdd_rw, fp_dpool_sdd)

// =====================================================================================================================
// mstream.obj: a memory stream (data, size) and pointers into it (position, overflow flag). A read or write that
// doesn't fit sets the flag and does nothing.
// =====================================================================================================================

// MemStreamCreate (0x4d8b80)
static MemStream* __cdecl MemStreamCreate_rw(int32_t n) {
    MemStream* s = (MemStream*)MemAlloc(8);
    if (!s) return 0;
    s->size = n;
    s->data = (uint8_t*)MemAlloc(n);
    return s;
}
static void fp_ms_create(Footprint& f, int32_t) { f.replay_only = "it allocates the stream"; }
PORT_FN(0x004d8b80, "MemStreamCreate", MemStreamCreate_rw, fp_ms_create)

// MemStreamDestroy (0x4d8bb0)
static void __cdecl MemStreamDestroy_rw(MemStream* s) {
    if (!s) return;
    OperatorDelete(s->data);
    s->size = 0;
    OperatorDelete(s);
}
static void fp_ms_destroy(Footprint& f, MemStream*) { f.replay_only = "it frees the stream"; }
PORT_FN(0x004d8bb0, "MemStreamDestroy", MemStreamDestroy_rw, fp_ms_destroy)

// MemStreamCreatePtr (0x4d8be0)
static MemStreamPtr* __cdecl MemStreamCreatePtr_rw(MemStream* s) {
    MemStreamPtr* p = (MemStreamPtr*)MemAlloc(0xc);
    if (!p) return 0;
    p->s = s;
    p->pos = 0;
    p->overflow = 0;
    return p;
}
static void fp_ms_create_ptr(Footprint& f, MemStream*) { f.replay_only = "it allocates the pointer"; }
PORT_FN(0x004d8be0, "MemStreamCreatePtr", MemStreamCreatePtr_rw, fp_ms_create_ptr)

// MemStreamDestroyPtr (0x4d8c00)
static void __cdecl MemStreamDestroyPtr_rw(MemStreamPtr* p) {
    if (!p) return;
    p->s = 0;
    p->pos = 0x2bad2bad;
    OperatorDelete(p);
}
static void fp_ms_destroy_ptr(Footprint& f, MemStreamPtr*) { f.replay_only = "it frees the pointer"; }
PORT_FN(0x004d8c00, "MemStreamDestroyPtr", MemStreamDestroyPtr_rw, fp_ms_destroy_ptr)

// MemStreamWrite / MemStreamRead (0x4d8c20 / 0x4d8c40): the whole stream to / from a file; always 1
static uint8_t __cdecl MemStreamWrite_rw(MemStream* s, int32_t file) {
    FileWrite(file, s->data, s->size);
    return 1;
}
static uint8_t __cdecl MemStreamRead_rw(MemStream* s, int32_t file) {
    FileReadExact(file, s->data, s->size);
    return 1;
}
static void fp_ms_file(Footprint& f, MemStream*, int32_t) { f.replay_only = "file I/O"; }
PORT_FN(0x004d8c20, "MemStreamWrite", MemStreamWrite_rw, fp_ms_file)
PORT_FN(0x004d8c40, "MemStreamRead", MemStreamRead_rw, fp_ms_file)

// the seeks and the small queries
static void fp_ms_ptr(Footprint& f, MemStreamPtr* p) { f.add(p, 0xc, "stream pointer"); }
static void __cdecl MemStreamSeekStart_rw(MemStreamPtr* p) { p->pos = 0; }
static void __cdecl MemStreamSeekEnd_rw(MemStreamPtr* p) { p->pos = p->s->size; }
static void __cdecl MemStreamSeekPos_rw(MemStreamPtr* p, int32_t pos) { p->pos = pos; }
static int32_t __cdecl MemStreamGetPos_rw(MemStreamPtr* p) { return p->pos; }
static uint8_t __cdecl MemStreamValidPos_rw(MemStreamPtr* p) { return p->pos >= 0 && p->s->size > p->pos ? 1 : 0; }
static uint8_t __cdecl MemStreamGotOverflow_rw(MemStreamPtr* p) {
    if (!p->overflow) return 0;
    p->overflow = 0;
    return 1;
}
static void fp_ms_seek_pos(Footprint& f, MemStreamPtr* p, int32_t) { fp_ms_ptr(f, p); }
static void fp_ms_query(Footprint&, MemStreamPtr*) {}
PORT_FN(0x004d8c60, "MemStreamSeekStart", MemStreamSeekStart_rw, fp_ms_ptr)
PORT_FN(0x004d8c70, "MemStreamSeekEnd", MemStreamSeekEnd_rw, fp_ms_ptr)
PORT_FN(0x004d8c80, "MemStreamSeekPos", MemStreamSeekPos_rw, fp_ms_seek_pos)
PORT_FN(0x004d8c90, "MemStreamGetPos", MemStreamGetPos_rw, fp_ms_query)
PORT_FN(0x004d8ca0, "MemStreamValidPos", MemStreamValidPos_rw, fp_ms_query)
PORT_FN(0x004d8cc0, "MemStreamGotOverflow", MemStreamGotOverflow_rw, fp_ms_ptr)

// what a put of n bytes at the position writes (when it fits). The functions' own check is pos >= 0 and
// pos + n <= size in 32 bits; a position near INT_MAX wraps it and they write ~2 GB away (and fault), which no
// footprint can list -- the footprints check without wrapping, so they never read out of range themselves.
static bool ms_fits(const MemStreamPtr* p, int64_t pos, int64_t n) { return n > 0 && pos >= 0 && pos + n <= p->s->size; }
static void fp_ms_put(Footprint& f, MemStreamPtr* p, int32_t n) {
    fp_ms_ptr(f, p);
    if (ms_fits(p, p->pos, n)) f.add(p->s->data + (uint32_t)p->pos, (uint32_t)n, "stream data");
}

// MemStreamPutInt / MemStreamPutReal (0x4d8ce0 / 0x4d8d10): a dword (the float's bits, moved as an integer)
static void __cdecl MemStreamPutInt_rw(MemStreamPtr* p, int32_t v) {
    const int32_t pos = p->pos;
    if (pos >= 0 && p->s->size >= (int32_t)((uint32_t)pos + 4u)) {
        *(volatile int32_t*)(p->s->data + (uint32_t)pos) = v;
        p->pos += 4;
        return;
    }
    p->overflow = 1;
}
static void fp_ms_put_int(Footprint& f, MemStreamPtr* p, int32_t) { fp_ms_put(f, p, 4); }
PORT_FN(0x004d8ce0, "MemStreamPutInt", MemStreamPutInt_rw, fp_ms_put_int)
static void __cdecl MemStreamPutReal_rw(MemStreamPtr* p, uint32_t bits) {
    const int32_t pos = p->pos;
    if (pos >= 0 && p->s->size >= (int32_t)((uint32_t)pos + 4u)) {
        *(volatile uint32_t*)(p->s->data + (uint32_t)pos) = bits;
        p->pos += 4;
        return;
    }
    p->overflow = 1;
}
static void fp_ms_put_real(Footprint& f, MemStreamPtr* p, uint32_t) { fp_ms_put(f, p, 4); }
PORT_FN(0x004d8d10, "MemStreamPutReal", MemStreamPutReal_rw, fp_ms_put_real)

// MemStreamPutString (0x4d8d40): the length, then the characters (no terminator); the length measured again
static void __cdecl MemStreamPutString_rw(MemStreamPtr* p, const char* s) {
    MemStreamPutInt(p, (int32_t)strlen(s));
    MemStreamPutData(p, s, (int32_t)strlen(s));
}
static void fp_ms_put_string(Footprint& f, MemStreamPtr* p, const char* s) {
    fp_ms_put(f, p, 4);
    // the characters go after the length -- or where the length would have gone, if it didn't fit
    const int64_t pos = ms_fits(p, p->pos, 4) ? (int64_t)p->pos + 4 : (int64_t)p->pos;
    const int32_t n = (int32_t)strlen(s);
    if (ms_fits(p, pos, n)) f.add(p->s->data + (uint32_t)pos, (uint32_t)n, "stream data");
}
PORT_FN(0x004d8d40, "MemStreamPutString", MemStreamPutString_rw, fp_ms_put_string)

// MemStreamPutData (0x4d8d80)
static void __cdecl MemStreamPutData_rw(MemStreamPtr* p, const void* src, int32_t n) {
    const int32_t pos = p->pos;
    if (pos >= 0 && !((int32_t)((uint32_t)pos + (uint32_t)n) > p->s->size)) {
        rep_movs(p->s->data + (uint32_t)pos, src, (uint32_t)n);
        p->pos += n;
        return;
    }
    p->overflow = 1;
}
static void fp_ms_put_data(Footprint& f, MemStreamPtr* p, const void*, int32_t n) { fp_ms_put(f, p, n); }
PORT_FN(0x004d8d80, "MemStreamPutData", MemStreamPutData_rw, fp_ms_put_data)

// MemStreamGetInt / MemStreamGetReal (0x4d8dc0 / 0x4d8df0)
static void __cdecl MemStreamGetInt_rw(MemStreamPtr* p, int32_t* out) {
    const int32_t pos = p->pos;
    if (pos >= 0 && p->s->size >= (int32_t)((uint32_t)pos + 4u)) {
        const int32_t v = *(volatile int32_t*)(p->s->data + (uint32_t)pos);
        *(volatile int32_t*)out = v;
        p->pos += 4;
        return;
    }
    p->overflow = 1;
}
static void fp_ms_get4(Footprint& f, MemStreamPtr* p, int32_t* out) {
    fp_ms_ptr(f, p);
    f.add(out, 4, "out");
}
PORT_FN(0x004d8dc0, "MemStreamGetInt", MemStreamGetInt_rw, fp_ms_get4)
static void __cdecl MemStreamGetReal_rw(MemStreamPtr* p, uint32_t* out) {
    const int32_t pos = p->pos;
    if (pos >= 0 && p->s->size >= (int32_t)((uint32_t)pos + 4u)) {
        const uint32_t v = *(volatile uint32_t*)(p->s->data + (uint32_t)pos);
        *(volatile uint32_t*)out = v;
        p->pos += 4;
        return;
    }
    p->overflow = 1;
}
static void fp_ms_get_real(Footprint& f, MemStreamPtr* p, uint32_t* out) { fp_ms_get4(f, p, (int32_t*)out); }
PORT_FN(0x004d8df0, "MemStreamGetReal", MemStreamGetReal_rw, fp_ms_get_real)

// MemStreamGetString (0x4d8e20): the length, then that many characters -- the buffer's size is ignored, nothing
// terminates the string, and when the length can't be read (the stream is exhausted) it's whatever the stack held.
// That uninitialised local is the dword just below the return address (sub esp, 4), which a C function's prologue
// would overwrite before it could be read, so this one is written in the original's frame layout: the same two
// calls by address, the length read from that slot.
static __declspec(naked) void __cdecl MemStreamGetString_rw(MemStreamPtr*, char*, int32_t) {
    __asm {
        sub esp, 4
        lea eax, [esp]
        push esi
        mov esi, [esp + 0xc]                // p
        push eax                            // &len
        push esi
        mov eax, 0x004d8dc0                 // MemStreamGetInt
        call eax
        mov ecx, [esp + 0xc]                // len
        mov eax, [esp + 0x18]               // buf
        add esp, 8
        push ecx
        push eax
        push esi
        mov eax, 0x004d8e50                 // MemStreamGetData
        call eax
        add esp, 0xc
        pop esi
        add esp, 4
        ret
    }
}
static void fp_ms_get_string(Footprint& f, MemStreamPtr* p, char* buf, int32_t) {
    fp_ms_ptr(f, p);
    if (ms_fits(p, p->pos, 4)) {
        const int32_t len = *(const int32_t*)(p->s->data + (uint32_t)p->pos);
        if (ms_fits(p, (int64_t)p->pos + 4, len)) f.add(buf, (uint32_t)len, "string");
    }
}
PORT_FN(0x004d8e20, "MemStreamGetString", MemStreamGetString_rw, fp_ms_get_string)

// MemStreamGetData (0x4d8e50)
static void __cdecl MemStreamGetData_rw(MemStreamPtr* p, void* dst, int32_t n) {
    const int32_t pos = p->pos;
    if (pos >= 0 && !((int32_t)((uint32_t)pos + (uint32_t)n) > p->s->size)) {
        rep_movs(dst, p->s->data + (uint32_t)pos, (uint32_t)n);
        p->pos += n;
        return;
    }
    p->overflow = 1;
}
static void fp_ms_get_data(Footprint& f, MemStreamPtr* p, void* dst, int32_t n) {
    fp_ms_ptr(f, p);
    if (ms_fits(p, p->pos, n)) f.add(dst, (uint32_t)n, "data");
}
PORT_FN(0x004d8e50, "MemStreamGetData", MemStreamGetData_rw, fp_ms_get_data)

// =====================================================================================================================
// slerp.obj
// =====================================================================================================================

// fld dword [p]; fchs; fstp dword [p] -- a negation through the FPU (a signalling NaN comes out quiet)
static __forceinline void fneg_store(float* p) {
    __asm { mov eax, p
            fld dword ptr [eax]
            fchs
            fstp dword ptr [eax] }
}
// omega = fpatan(sinom, cosom) (full precision, never stored), then sin((1 - t) omega) / sinom and
// sin(t omega) / sinom: products rounded to the FPU's precision, so doubles hold them
static void slerp_scales(float sinom, float cosom, float t, double* s0, double* s1) {
    float one = 1.0f;
    double a, b;
    __asm { fld sinom
            fld cosom
            fpatan
            fld sinom
            fdivr one
            fld one
            fsub t
            fmul st, st(2)
            fsin
            fmul st, st(1)
            fxch st(2)
            fmul t
            fsin
            fmulp st(1), st
            fstp b
            fstp a }
    *s0 = a;
    *s1 = b;
}

// Slerp (0x4d8eb0): out = the spherical interpolation from a to b at t, normalised. When a.b < 0 the input a is
// negated IN PLACE (it's declared const); when sin(omega) is below FLT_EPSILON, out = a.
static void __cdecl Slerp_rw(Q4* out, Q4* a, const Q4* b, float t) {
    const double c = ((D(b->x) * a->x + D(b->y) * a->y) + D(b->w) * a->w) + D(a->z) * b->z;
    float cosom = (float)c;
    if (!(c >= 0.0f)) {                                      // fcom 0.0f; test ah, 1
        fneg_store(&a->x);
        fneg_store(&a->y);
        fneg_store(&a->z);
        fneg_store(&a->w);
        cosom = -cosom;
    }
    const double sr = x87_sqrt(fabs(1.0f - D(cosom) * cosom));
    const float sinom = (float)sr;
    if (!(fabs(sr) >= (double)Fb(0x34000000))) {             // FLT_EPSILON; fcomp; test ah, 1
        memcpy(out, a, 16);
        return;
    }
    double s0, s1;
    slerp_scales(sinom, cosom, t, &s0, &s1);
    const float ay = (float)(D(a->y) * s0), az = (float)(D(a->z) * s0), aw = (float)(D(a->w) * s0);
    const float by = (float)(D(b->y) * s1), bz = (float)(D(b->z) * s1), bw = (float)(D(b->w) * s1);
    out->x = (float)(s1 * b->x + D(a->x) * s0);
    out->y = (float)(D(by) + ay);
    out->z = (float)(D(bz) + az);
    out->w = (float)(D(bw) + aw);
    const double k = 1.0f / x87_sqrt(((D(out->y) * out->y + D(out->z) * out->z) + D(out->w) * out->w) +
                                     D(out->x) * out->x);
    out->x = (float)(D(out->x) * k);
    out->y = (float)(D(out->y) * k);
    out->z = (float)(D(out->z) * k);
    out->w = (float)(k * out->w);
}
static void fp_slerp(Footprint& f, Q4* out, Q4* a, const Q4*, float) {
    f.add(out, 16, "out");
    f.add(a, 16, "a (negated in place)");
    f.pure = true;
}
PORT_FN(0x004d8eb0, "Slerp", Slerp_rw, fp_slerp)

// =====================================================================================================================
// bag.obj: an unordered array of pointers with a fixed capacity
// =====================================================================================================================

// BagBase::BagBase (0x4d97c0)
static BagBase* __fastcall BagBase_ctor_rw(BagBase* self, Edx, const char* name, int32_t cap) {
    self->name = name;
    self->count = 0;
    self->crashes = 1;
    self->cap = cap;
    self->items = (void**)MemAlloc((int)((uint32_t)cap << 2));
    return self;
}
static void fp_bag_ctor(Footprint& f, BagBase*, Edx, const char*, int32_t) { f.replay_only = "it allocates the bag"; }
PORT_FN(0x004d97c0, "BagBase::BagBase", BagBase_ctor_rw, fp_bag_ctor)

// BagBase::~BagBase (0x4d97f0)
static void __fastcall BagBase_dtor_rw(BagBase* self, Edx) {
    OperatorDelete(self->items);
    self->items = 0;
}
static void fp_bag_dtor(Footprint& f, BagBase*, Edx) { f.replay_only = "it frees the bag"; }
PORT_FN(0x004d97f0, "BagBase::~BagBase", BagBase_dtor_rw, fp_bag_dtor)

// BagBase::add (0x4d9810): full: 0, and a panic if it crashes
static uint8_t __fastcall BagBase_add_rw(BagBase* self, Edx, void* p) {
    const int32_t n = self->count;
    if (self->cap > n) {
        self->items[n] = p;
        self->count++;
        return 1;
    }
    if (self->crashes) LogPanic(k_fmt_bag_full, self->name);
    return 0;
}
static void fp_bag_add(Footprint& f, BagBase* self, Edx, void*) {
    f.add(self, 0x14, "bag");
    if (self->cap > self->count) f.add(self->items + self->count, 4, "bag slot");
}
PORT_FN(0x004d9810, "BagBase::add", BagBase_add_rw, fp_bag_add)

// BagBase::remove_all (0x4d9850)
static void __fastcall BagBase_remove_all_rw(BagBase* self, Edx) { self->count = 0; }
static void fp_bag_remove_all(Footprint& f, BagBase* self, Edx) { f.add(self, 0x14, "bag"); }
PORT_FN(0x004d9850, "BagBase::remove_all", BagBase_remove_all_rw, fp_bag_remove_all)

// BagBase::remove (0x4d9860): the last item moved into the removed one's place; unknown: a panic
static void __fastcall BagBase_remove_rw(BagBase* self, Edx, void* p) {
    int32_t n = self->count;
    int32_t i = 0;
    if (n > 0) {
        void** const items = self->items;
        for (; i < n; i++)
            if (items[i] == p) {
                n--;
                self->count = n;
                items[i] = items[n];
                return;
            }
    }
    LogPanic(k_fmt_bag_unknown, self->name);
}
static void fp_bag_remove(Footprint& f, BagBase* self, Edx, void*) {
    f.add(self, 0x14, "bag");
    if (self->count > 0) f.add(self->items, 4u * (uint32_t)self->count, "bag items");
}
PORT_FN(0x004d9860, "BagBase::remove", BagBase_remove_rw, fp_bag_remove)

// =====================================================================================================================
// clpboard.obj: through the game's imports (the window from Win32GetWindow)
// =====================================================================================================================

// ClipboardGetText (0x4d98d0): CF_TEXT, up to n - 1 characters and a terminator
static uint8_t __cdecl ClipboardGetText_rw(char* buf, int32_t n) {
    uint8_t ok = 0;
    if (IAT(OpenClipboard_t, IAT_OpenClipboard)(Win32GetWindow())) {
        if (void* h = IAT(GetClipboardData_t, IAT_GetClipboardData)(1)) {
            if (char* p = (char*)IAT(GlobalLock_t, IAT_GlobalLock)(h)) {
                game_strncpy(buf, p, n);
                IAT(GlobalUnlock_t, IAT_GlobalUnlock)(p);        // the pointer, not the handle (as the original)
                buf[n - 1] = 0;
                ok = 1;
            }
        }
        IAT(CloseClipboard_t, IAT_CloseClipboard)();
    }
    return ok;
}
static void fp_clip_get(Footprint& f, char*, int32_t) { f.replay_only = "the clipboard"; }
PORT_FN(0x004d98d0, "ClipboardGetText", ClipboardGetText_rw, fp_clip_get)

// ClipboardPutText (0x4d9930): a moveable, shared copy with its terminator, as CF_TEXT
static uint8_t __cdecl ClipboardPutText_rw(const char* s) {
    uint8_t ok = 0;
    const uint32_t n = (uint32_t)strlen(s) + 1;
    if (IAT(OpenClipboard_t, IAT_OpenClipboard)(Win32GetWindow())) {
        if (IAT(EmptyClipboard_t, IAT_EmptyClipboard)()) {
            if (void* h = IAT(GlobalAlloc_t, IAT_GlobalAlloc)(0x2002, n)) {   // GMEM_MOVEABLE | GMEM_DDESHARE
                void* p = IAT(GlobalLock_t, IAT_GlobalLock)(h);
                rep_movs(p, s, n);
                IAT(GlobalUnlock_t, IAT_GlobalUnlock)(h);
                IAT(SetClipboardData_t, IAT_SetClipboardData)(1, h);
                ok = 1;
            }
        }
        IAT(CloseClipboard_t, IAT_CloseClipboard)();
    }
    return ok;
}
static void fp_clip_put(Footprint& f, const char*) { f.replay_only = "the clipboard"; }
PORT_FN(0x004d9930, "ClipboardPutText", ClipboardPutText_rw, fp_clip_put)

// =====================================================================================================================
// html.obj: an HTML log file. Each line is formatted into a 0x400-byte buffer whose top half is also FilePrintf's
// scratch buffer, so a line over 0x1ff characters is overwritten while it's written (and one over 0x3ff overruns
// the stack).
// =====================================================================================================================

// HTMLBegin (0x4d99e0)
static uint8_t __cdecl HTMLBegin_rw(const char* file, const char* title) {
    const int32_t h = FileCreate(file);
    G32(A_HTML_FILE) = h;
    if (h) {
        HTMLWriteLn(k_fmt_html_head, title);
        HTMLWriteLn(k_str_html_body);
        return 1;
    }
    LogReport(k_fmt_html_cant, file);
    return 0;
}
static void fp_html_begin(Footprint& f, const char*, const char*) { f.replay_only = "it creates a file"; }
PORT_FN(0x004d99e0, "HTMLBegin", HTMLBegin_rw, fp_html_begin)

// HTMLEnd (0x4d9a30)
static void __cdecl HTMLEnd_rw() {
    HTMLWriteLn(k_str_html_end);
    FileClose(&G32(A_HTML_FILE));
    G32(A_HTML_FILE) = 0;
}
static void fp_html_end(Footprint& f) { f.replay_only = "it writes and closes a file"; }
PORT_FN(0x004d9a30, "HTMLEnd", HTMLEnd_rw, fp_html_end)

// HTMLWriteLn (0x4d9a60) -- variadic in the original: see the note at the top
#define HTML_VA uint32_t a0, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, \
                uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t
static void __cdecl HTMLWriteLn_rw(const char* fmt, HTML_VA) {
    char buf[0x400];
    game_vsprintf(buf, fmt, (va_list)&a0);
    FilePrintf(G32(A_HTML_FILE), buf + 0x200, k_fmt_html_line, buf);
}
#define HTML_VA_FP uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, \
                   uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t
static void fp_html_writeln(Footprint& f, const char*, HTML_VA_FP) { f.replay_only = "it writes a file"; }
PORT_FN(0x004d9a60, "HTMLWriteLn", HTMLWriteLn_rw, fp_html_writeln)

// HTMLWrite (0x4d9ab0): the formatted text between the style's opening and closing strings
static void __cdecl HTMLWrite_rw(const HTMLStyle* style, const char* fmt, HTML_VA) {
    char buf[0x400];
    game_vsprintf(buf, fmt, (va_list)&a0);
    FilePrintf(G32(A_HTML_FILE), buf + 0x200, k_fmt_html_styled, style->open, buf, style->close);
}
static void fp_html_write(Footprint& f, const HTMLStyle*, const char*, HTML_VA_FP) { f.replay_only = "it writes a file"; }
PORT_FN(0x004d9ab0, "HTMLWrite", HTMLWrite_rw, fp_html_write)

// HTMLEnter / HTMLLeave (0x4d9b00 / 0x4d9b10): the style's opening / closing string, as a format
static void __cdecl HTMLEnter_rw(const HTMLStyle* style) { HTMLWriteLn(style->open); }
static void __cdecl HTMLLeave_rw(const HTMLStyle* style) { HTMLWriteLn(style->close); }
static void fp_html_style(Footprint& f, const HTMLStyle*) { f.replay_only = "it writes a file"; }
PORT_FN(0x004d9b00, "HTMLEnter", HTMLEnter_rw, fp_html_style)
PORT_FN(0x004d9b10, "HTMLLeave", HTMLLeave_rw, fp_html_style)
