// phys_player.cpp -- M3 3.4 group P: the player's car and the input side, rewritten.
//
//   playcar.obj   PlayCar: the LocalCar the player drives. Update reads the driver's controls through the
//                 13 DriverGet* functions (hooked by the recorder, hook/replay.cpp, so they stay the game's
//                 own and are called here by address), sets them on the Car, runs Car::Update, fills the
//                 telemetry and runs the spotter (which cars are alongside).
//   localcar.obj  LocalCar: a Car simulated on this machine (the player's, or a multiplayer peer's copy),
//                 with the teleport flag and the network packet.
//   driver.obj    the input driver, MAIN thread: DriverUpdate turns the mapped controls into steering /
//                 throttle / brake / clutch / gear with rate limits and the option scalings. Its state is
//                 the statics 0x5221c8..0x5222e0 (the layout is below).
//   control.obj   control mapping, MAIN thread: a Control is {type, key or value}; ControlReadAnalog turns
//                 one into 0..1 (or -1..1) from the keyboard state, the JoyPos and the mouse; the name
//                 table (83 entries, control_init) maps controls to their ini strings and display names.
//                 Its statics are 0x5224a8..0x522864 and the table 0x4ee480..0x4eec48.
//
// The 13 DriverGet* readers (0x441df0, 0x441e60..0x441f50) are NOT here: the recorder hooks those addresses.
//
// Threads (docs/PORTING.md): the driver and control functions run on the main thread, so their footprints
// list every static they write; the PlayCar / LocalCar methods run on the physics thread, whose statics a
// shadow check saves itself -- except the driver's own statics that PlayCar::Update writes through
// DriverSetForce / DriverSetGear, which are listed.
//
// Written from the v1.0 disassembly: register values as double, stored values as float, the same grouping,
// integer compares of float bits done on the bits, every call in the original's order.
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "viperport.h"
#include "port.h"
#include "x87.h"
#include "phys_types.h"

#define D(x) ((double)(x))                          // a register value (x87.h)
typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

static inline uint32_t bits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static inline float as_float(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
template <typename T> static inline T& at(void* o, uint32_t off) { return *(T*)((uint8_t*)o + off); }
template <typename T> static inline T* P(uint32_t addr) { return (T*)(uintptr_t)addr; }

// (0 < v, or unordered) ? v : 0 -- the "fld 0; fcom st(1); test ah,1; jne; fxch; fstp st(0)" clamp
static inline double max0_nan(double v) { return (0.0 >= v) ? 0.0 : v; }
// (0 > v) ? v : 0 -- the "fld 0; fcom st(1); test ah,0x41; je; fxch; fstp st(0)" clamp (NaN -> 0)
static inline double min0(double v) { return (0.0 > v) ? v : 0.0; }

namespace {

// ---- control.obj's types ------------------------------------------------------------------------------------
struct Control {                                    // 8 bytes: a mapped input
    int32_t type;                                   // 1 key, 2 joystick axis, 3 joystick button, 4 POV hat,
                                                    // 5 mouse axis, 6 mouse button; anything else reads 0
    union { uint8_t key; int32_t value; };          // the scan code (type 1: only this byte is ever written)
                                                    // or the axis / button / hat direction
};
static_assert(sizeof(Control) == 8, "Control");

struct ControlEntry {                               // 24 bytes: a row of control_init's table
    const char* name;                               // the ini string ("Joy AX+", "Key_A", ...)
    const char* display;                            // the translated display name, or 0: inline_display
    int32_t type;
    int32_t value;                                  // a key's scan code is the low byte
    char inline_display[4];                         // a plain key's own letter, KeyConvertScanKey
    uint8_t reserved;                               // 1: ControlDetect never offers it (Key_SPACE)
    uint8_t _15[3];
};
static_assert(sizeof(ControlEntry) == 24, "ControlEntry");

struct Xlator {                                     // a translated string (xlator.obj): 12 bytes, 16 apart
    const char* key;
    const char* text;                               // xlate: lookup(key), or the "missing" string
    uint32_t cookie;                                // Xlator::g_cookie when translated (0x4eb108)
};

// as JoyGetPos fills it (60 bytes): six axes in -1..1, 32 buttons, the hat as an INT -- 0 centred, else
// degrees / 90 + 1 (hook/platform.cpp's SDL JoyGetPos stores a float there, which never matches a hat control)
struct JoyPos { float axis[6]; uint8_t button[32]; int32_t pov; };
static_assert(sizeof(JoyPos) == 60, "JoyPos");
struct Mouse { float x, y; uint8_t button[3]; uint8_t _0b; };       // 12 bytes, copied as three dwords
static_assert(sizeof(Mouse) == 12, "Mouse");

// ---- the statics --------------------------------------------------------------------------------------------
// driver.obj, 0x5221c8..0x5222e0 (from DriverBegin/DriverRefresh/DriverUpdate's stores and the option names)
enum : uint32_t {
    DRV_BASE = 0x005221c8, DRV_SIZE = 0x118,
    S_BRAKING = 0x005221c8,          // float   DriverGetBraking
    S_CTL_THROTTLE = 0x005221d0,     // Control 'throttle'
    S_BRAKE_RANGE = 0x005221d8,      // float   option brake_range
    S_CTL_STEER_LEFT = 0x005221e0,   // Control 'steer_left'
    S_PITCH = 0x005221e8,            // float   DriverGetPitch: downshift - upshift (a plane's elevator)
    S_LOOK_SIDE = 0x005221ec,        // float   DriverGetLookSide: look_right - look_left
    S_BRAKE_SPEED = 0x005221f0,      // float   option brake_speed (digital ramp)
    S_THROTTLE_RANGE = 0x005221f4,   // float   option throttle_range
    S_THROTTLE_SPEED = 0x005221f8,   // float   option throttle_speed
    S_SHIFT_LATCH = 0x005221fc,      // u8      a shift key is held (one gear per press)
    S_STEERING = 0x00522200,         // float   -1..1
    S_NUM_GEARS = 0x00522204,        // int     DriverSetNumGears
    S_AIRLIFT = 0x00522208,          // u8
    S_CTL_BRAKE = 0x00522210,        // Control 'braking'
    S_THROTTLE_SENS = 0x00522218,    // float   option throttle_sensitivity
    S_CTL_LOOK_RIGHT = 0x00522220,   // Control 'look_right'
    S_BRAKE_SENS = 0x00522228,       // float   option brake_sensitivity
    S_FORCE_A = 0x0052222c,          // float   DriverSetForce arg 1 (steer_feedback)
    S_FORCE_B = 0x00522230,          // float   arg 2 (long_g)
    S_FORCE_FEEDBACK = 0x00522234,   // u8      option force_feedback
    S_STEER_RANGE = 0x00522238,      // float   option steer_range
    S_CTL_HORN = 0x00522240,         // Control 'horn'
    S_GAME_PAD = 0x00522248,         // u8      option game_pad: every axis digital
    S_FORCE_C = 0x0052224c,          // float   arg 3 (vert_g)
    S_STEER_SPEED = 0x00522250,      // float   option steer_speed
    S_CTL_EBRAKE = 0x00522258,       // Control 'e_brake'
    S_EBRAKE = 0x00522260,           // float
    S_GEAR = 0x00522264,             // int     -1 R, 0 N, 1..num_gears
    S_CTL_STEER_RIGHT = 0x00522268,  // Control 'steer_right'
    S_CLUTCH = 0x00522270,           // float   0..1 pedal (DriverGetClutch maps it to engagement)
    S_HELP = 0x00522274,             // u8
    S_CTL_HELP = 0x00522278,         // Control 'help'
    S_CTL_DOWNSHIFT = 0x00522280,    // Control 'downshift'
    S_CTL_LOOK_BACK = 0x00522288,    // Control 'look_back'
    S_CTL_AIRLIFT = 0x00522290,      // Control 'airlift'
    S_REVERSE = 0x00522298,          // u8
    S_THROTTLE = 0x0052229c,         // float
    S_HORN = 0x005222a0,             // u8
    S_NONLINEAR = 0x005222a4,        // u8      option nonlinear_steering (DriverGetSteering: |s| s)
    S_CTL_LOOK_LEFT = 0x005222a8,    // Control 'look_left'
    S_CTL_REVERSE = 0x005222b0,      // Control 'reverse'
    S_CTL_UPSHIFT = 0x005222c0,      // Control 'upshift'
    S_LOOK_BACK = 0x005222c8,        // u8
    S_FF_TIMER = 0x005222cc,         // float   time since the last JoySetForce (every 1/18 s)
    S_STEER_SENS = 0x005222d0,       // float   option steer_sensitivity (1 - it is the analog dead band)
    S_CTL_CLUTCH = 0x005222d8,       // Control 'clutch'
};
// control.obj, 0x5224a8..0x522864: 44 Xlators (16 apart) with the mouse, the neutral copies and the
// joystick between them; the table itself is in .data at 0x4ee480
enum : uint32_t {
    CTL_BASE = 0x005224a8, CTL_SIZE = 0x3bc,
    S_MOUSE = 0x00522508,            // Mouse   ControlUpdate: x, y in -1..1 of the half screen, 3 buttons
    S_NEUTRAL_JOY = 0x005225b0,      // JoyPos  ControlDetectInit's copy
    S_INIT_GUARD = 0x0052260c,       // u8      bit 0: control_init has built the table
    S_NUM_ENTRIES = 0x0052261c,      // int     83
    S_XL_UNKNOWN = 0x00522638,       // Xlator  ControlToDisplayString's text for an unmapped control
    S_NEUTRAL_MOUSE = 0x00522700,    // Mouse
    S_JOY = 0x00522730,              // JoyPos  ControlUpdate: JoyGetPos
    S_ENTRIES = 0x00522770,          // ControlEntry* = 0x4ee480
    TABLE = 0x004ee480, TABLE_SIZE = 83 * 24,
    S_EMPTY_STRING = 0x004ee47c,     // "" (ControlGet's buffer initialiser)
    S_XLATOR_COOKIE = 0x004eb108,    // Xlator::g_cookie
    S_KEYBOARD = 0x00508088,         // u8[256] the DirectInput keyboard state (ScanUpdate), bit 7 down, bit 0 hit
};

// ---- Car / LocalCar / PlayCar field offsets (out/types.tsv) --------------------------------------------------
enum : uint32_t {
    C_STATUS = 0x004,                // PhobRoot: PlayCar::Update's pedal status for MakeStatusString (1 brake, 2 gas)
    C_MSG_OFFSET = 0x008,            // PhobRoot: this object's offset in the message buffer
    C_ROT = 0x038, C_POS = 0x05c,    // frame
    C_VELOCITY = 0x234,
    C_LOOK_BACK = 0x4ec, C_LOOK_SIDE = 0x4f0, C_REALISM = 0x4f4, C_YAW_CONTROL = 0x4f8, C_RACE_STATE = 0x4fc,
    C_STEERING = 0x500, C_CAR_INDEX = 0x510, C_AUTO_CLUTCH = 0x534,
    C_WHEELS = 0x554, WHEEL_SIZE = 0x1a4, W_FLAG20 = 0x20, W_DIGITAL_THROTTLE = 0x160,
    C_IS_PLANE = 0xbe4, C_ENGINE = 0xbfc, C_ENGINE_SMOOTHED_THROTTLE = 0xc34, C_ENGINE_STALLED = 0xc3d,
    C_CLUTCH_ENGAGEMENT = 0xc7c, C_TRANS_NUM_GEARS = 0xcb8,
    C_GEAR = 0xd64, C_PERCEIVED_RPM = 0xd70, C_DIGITAL_THROTTLE = 0xd74,
    C_SPEED = 0xddc, C_THROTTLE = 0xde0, C_BRAKING = 0xde4, C_HORN = 0xe28,
    C_LAT_G = 0xea4, C_LONG_G = 0xea8, C_VERT_G = 0xeac, C_STEER_FEEDBACK = 0xeb0,
    // LocalCar
    L_TELEPORTED = 0xec0, L_EC4 = 0xec4,
    // PlayCar
    PC_AUTO_SHIFT = 0xec8, PC_AUTO_CLUTCH = 0xec9, PC_AIRLIFT_TIME = 0xecc,
    PC_SPOT_FLAGS = 0xed0, PC_SPOT_FLAGS_PREV = 0xed4,
    PC_SND_LEFT = 0xed8, PC_SND_RIGHT = 0xedc, PC_SND_BOTH = 0xee0, PC_SND_CLEAR = 0xee4,
    PC_SPOT_TIME = 0xee8, PC_SPOTTER = 0xeec,
    PLAYCAR_VTABLE = 0x004dc238, LOCALCAR_VTABLE = 0x004dc5f0,
};

}  // namespace

// ---- the game's functions these call (by v1.0 address) -----------------------------------------------------
typedef void*(__fastcall* CarCtor_t)(void*, Edx, void* data, void* p);
typedef void(__fastcall* CarVoid_t)(void*, Edx);
typedef void(__fastcall* CarSetF_t)(void*, Edx, float);
typedef void(__fastcall* CarSetI_t)(void*, Edx, int);
typedef void(__fastcall* CarSetB_t)(void*, Edx, unsigned char);
typedef void(__fastcall* CarTeleport_t)(void*, Edx, const void* frame, const void* point);
typedef void(__fastcall* CarGetMessage_t)(void*, Edx, uint8_t*);
typedef void(__cdecl* MatrixNormalize_t)(void*);
typedef int(__cdecl* PhysicsGetRealism_t)(void);
typedef float(__cdecl* PhysicsGetTime_t)(void);
typedef float(__cdecl* FloatGet_t)(void);
typedef uint32_t*(__cdecl* PhysTaskGetTelemetry_t)(void);
typedef void*(__cdecl* PhysTaskFindCar_t)(int);
typedef unsigned char(__cdecl* CanTeleport_t)(void*);
typedef void(__cdecl* RegisterCar_t)(void*, int);
typedef void(__cdecl* UnregisterCar1_t)(void*);
typedef void*(__cdecl* SoundDashCreate_t)(const char*, int, int);
typedef void(__cdecl* OptionsGetI_t)(const char*, const char*, int*);
typedef void(__cdecl* OptionsGetB_t)(const char*, const char*, unsigned char*);
typedef void(__cdecl* OptionsGetF_t)(const char*, const char*, float*);
typedef void(__cdecl* OptionsGetS_t)(const char*, const char*, char*, int);
typedef void(__cdecl* OptionsSetS_t)(const char*, const char*, const char*);
typedef float(__cdecl* DriverGetSteering_t)(float);
typedef int(__cdecl* IntGet_t)(void);
typedef unsigned char(__cdecl* ByteGet_t)(void);
typedef void(__cdecl* DriverSetForce_t)(uint32_t, uint32_t, uint32_t);   // floats moved as bits
typedef void(__cdecl* DriverSetInt_t)(int);
typedef void(__cdecl* JoyEnableFF_t)(unsigned char);
typedef void(__cdecl* JoySetForce_t)(uint32_t, uint32_t, uint32_t);
typedef unsigned char(__cdecl* JoyGetPos_t)(void*);
typedef void(__cdecl* MousePeek_t)(int*, int*, int*);
typedef unsigned char(__cdecl* Scan_t)(uint32_t);                        // ScanDown / ScanHit (a scan code)
typedef char(__cdecl* KeyConvertScanKey_t)(uint32_t);
typedef void(__fastcall* XlatorXlate_t)(void*, Edx);
typedef char*(__cdecl* strncpy_t)(char*, const char*, size_t);
typedef int(__cdecl* stricmp_t)(const char*, const char*);

static const CarCtor_t Car_Car = (CarCtor_t)0x004364c0;
static const CarVoid_t Car_dtor = (CarVoid_t)0x00436920;
static const CarCtor_t LocalCar_LocalCar = (CarCtor_t)0x004444e0;
static const CarVoid_t LocalCar_dtor_orig = (CarVoid_t)0x00444540;
static const CarTeleport_t LocalCar_Teleport_orig = (CarTeleport_t)0x00444560;
static const CarVoid_t Car_Reset = (CarVoid_t)0x00439f90;
static const CarTeleport_t Car_Teleport = (CarTeleport_t)0x00439b50;
static const CarVoid_t Car_Update = (CarVoid_t)0x00437680;
static const CarGetMessage_t Car_GetMessage = (CarGetMessage_t)0x00438ef0;
static const CarSetI_t Car_SetTractionControl = (CarSetI_t)0x004395e0;
static const CarSetI_t Car_SetABSBraking = (CarSetI_t)0x00439600;
static const CarSetF_t Car_SetSteering = (CarSetF_t)0x00439620;
static const CarSetF_t Car_SetThrottle = (CarSetF_t)0x004396d0;
static const CarSetF_t Car_SetBraking = (CarSetF_t)0x004396e0;
static const CarSetF_t Car_SetClutch = (CarSetF_t)0x004396f0;
static const CarSetF_t Car_SetEBrake = (CarSetF_t)0x00439700;
static const CarSetI_t Car_SetGear = (CarSetI_t)0x00439710;
static const CarVoid_t Car_SetGearAuto = (CarVoid_t)0x00439740;
static const CarVoid_t Car_HelpOut = (CarVoid_t)0x00439780;
static const CarSetB_t Car_SetHorn = (CarSetB_t)0x0043afb0;
static const CarSetF_t Car_SetPitch = (CarSetF_t)0x0043b050;
static const CarVoid_t Engine_Crank = (CarVoid_t)0x00446800;
static const MatrixNormalize_t MatrixNormalize = (MatrixNormalize_t)0x00429180;
static const PhysicsGetRealism_t PhysicsGetRealism = (PhysicsGetRealism_t)0x0042bd50;
static const PhysicsGetTime_t PhysicsGetTime = (PhysicsGetTime_t)0x0042bc80;
static const FloatGet_t HackThrottleBoost = (FloatGet_t)0x0040d4e0;
static const FloatGet_t HackGripBoost = (FloatGet_t)0x0040d500;
static const PhysTaskGetTelemetry_t PhysTaskGetTelemetry = (PhysTaskGetTelemetry_t)0x00428d80;
static const PhysTaskFindCar_t PhysTaskFindCar = (PhysTaskFindCar_t)0x00426d40;
static const CanTeleport_t CanTeleport = (CanTeleport_t)0x00443920;
static const RegisterCar_t AIRegisterPlayCar = (RegisterCar_t)0x0041d260;
static const RegisterCar_t AIUnregisterCar = (RegisterCar_t)0x0041d2e0;
static const RegisterCar_t MultiRegisterLocalCar = (RegisterCar_t)0x004a2760;
static const UnregisterCar1_t MultiUnregisterLocalCar = (UnregisterCar1_t)0x004a27e0;
static const SoundDashCreate_t SoundDash_Create = (SoundDashCreate_t)0x00472510;
static const OptionsGetI_t OptionsGetI = (OptionsGetI_t)0x004713e0;
static const OptionsGetB_t OptionsGetB = (OptionsGetB_t)0x00471470;
static const OptionsGetF_t OptionsGetF = (OptionsGetF_t)0x00471350;
static const OptionsGetS_t OptionsGetS = (OptionsGetS_t)0x00471500;
static const OptionsSetS_t OptionsSetS = (OptionsSetS_t)0x00471560;
// the recorder's hooks sit on these; calling the address reaches them (the controls are shadow inputs)
static const DriverGetSteering_t DriverGetSteering = (DriverGetSteering_t)0x00441df0;
static const FloatGet_t DriverGetThrottle = (FloatGet_t)0x00441e60;
static const FloatGet_t DriverGetBraking = (FloatGet_t)0x00441e70;
static const FloatGet_t DriverGetClutch = (FloatGet_t)0x00441e80;
static const FloatGet_t DriverGetEBrake = (FloatGet_t)0x00441ec0;
static const IntGet_t DriverGetGear = (IntGet_t)0x00441ed0;
static const ByteGet_t DriverGetHorn = (ByteGet_t)0x00441ef0;
static const ByteGet_t DriverGetAirlift = (ByteGet_t)0x00441f00;
static const ByteGet_t DriverGetReverse = (ByteGet_t)0x00441f10;
static const ByteGet_t DriverGetHelp = (ByteGet_t)0x00441f20;
static const FloatGet_t DriverGetLookSide = (FloatGet_t)0x00441f30;
static const FloatGet_t DriverGetPitch = (FloatGet_t)0x00441f40;
static const ByteGet_t DriverGetLookBack = (ByteGet_t)0x00441f50;
// this file's own functions, called by address so the hooked rewrite (or the original) is what runs
static const DriverSetForce_t DriverSetForceA = (DriverSetForce_t)0x00441db0;
static const DriverSetInt_t DriverSetNumGearsA = (DriverSetInt_t)0x00441dd0;
static const DriverSetInt_t DriverSetGearA = (DriverSetInt_t)0x00441ee0;
static const ByteGet_t DriverIsSteeringDigitalA = (ByteGet_t)0x00441f60;
static const ByteGet_t DriverIsThrottleDigitalA = (ByteGet_t)0x00441fc0;
typedef void(__cdecl* DriverVoid_t)(void);
static const DriverVoid_t DriverRefreshA = (DriverVoid_t)0x00441560;
typedef void(__cdecl* ControlUpdate_t)(unsigned char);
typedef void(__cdecl* ControlGet_t)(Control*, const char*);
typedef unsigned char(__cdecl* ControlPred_t)(const Control*);
typedef double(__cdecl* ControlReadAnalog_t)(const Control*);           // ST0, unrounded
typedef void(__cdecl* ControlToString_t)(char*, int, const Control*);
typedef void(__cdecl* ControlFromString_t)(Control*, const char*);
typedef void(__cdecl* ControlFromEntry_t)(Control*, ControlEntry*);
typedef const ControlEntry*(__cdecl* FindEntry_t)(const Control*);
typedef void(__cdecl* ControlInit_t)(void);
static const ControlUpdate_t ControlUpdateA = (ControlUpdate_t)0x0044b340;
static const ControlGet_t ControlGetA = (ControlGet_t)0x0044b430;
static const ControlInit_t control_initA = (ControlInit_t)0x0044b620;
static const ControlReadAnalog_t ControlReadAnalogA = (ControlReadAnalog_t)0x0044ceb0;
static const ControlReadAnalog_t get_neutral_value_analogA = (ControlReadAnalog_t)0x0044d310;
static const ControlPred_t get_neutral_value_digitalA = (ControlPred_t)0x0044d3c0;
static const ControlPred_t ControlReadDigitalA = (ControlPred_t)0x0044d470;
static const ControlPred_t ControlIsDigitalA = (ControlPred_t)0x0044d4c0;
static const ControlToString_t ControlToInternalStringA = (ControlToString_t)0x0044d4e0;
static const ControlFromString_t ControlFromInternalStringA = (ControlFromString_t)0x0044d530;
static const ControlFromEntry_t control_from_entryA = (ControlFromEntry_t)0x0044d590;
static const FindEntry_t find_control_entryA = (FindEntry_t)0x0044d5f0;
// the platform layer (some replaced by SDL in hook/platform.cpp -- always through the address)
static const JoyEnableFF_t JoyEnableForceFeedback = (JoyEnableFF_t)0x00419250;
static const JoySetForce_t JoySetForce = (JoySetForce_t)0x00419360;
static const JoyGetPos_t JoyGetPos = (JoyGetPos_t)0x00418ff0;
static const MousePeek_t MousePeek = (MousePeek_t)0x004145f0;
static const Scan_t ScanDown = (Scan_t)0x004130e0;
static const Scan_t ScanHit = (Scan_t)0x00413100;
static const KeyConvertScanKey_t KeyConvertScanKey = (KeyConvertScanKey_t)0x00413ed0;
static const XlatorXlate_t Xlator_xlate = (XlatorXlate_t)0x0041afb0;
static const strncpy_t g_strncpy = (strncpy_t)0x004cf3a0;               // the game's CRT
static const stricmp_t g_stricmp = (stricmp_t)0x004da350;
static void** const g_deity = (void**)0x005218ac;                         // class Deity* deity
static int* const gxScreenWid = (int*)0x005228f4;
static int* const gxScreenHit = (int*)0x005228d4;

// ---- footprint helpers ------------------------------------------------------------------------------------------
static void fp_driver_statics(Footprint& f) { f.add(P<void>(DRV_BASE), DRV_SIZE, "driver.obj statics"); }
static void fp_control_inputs(Footprint& f) {                            // what ControlUpdate fills
    f.add(P<void>(S_JOY), sizeof(JoyPos), "JoyPos");
    f.add(P<void>(S_MOUSE), sizeof(Mouse), "mouse");
}
static void fp_control_table(Footprint& f) {                             // what control_init builds
    f.add(P<void>(CTL_BASE), CTL_SIZE, "control.obj statics");
    f.add(P<void>(TABLE), TABLE_SIZE, "control table");
}

// =============================================================================================================
// control.obj
// =============================================================================================================

// ControlIsDigital: keys, buttons, the hat and mouse buttons are digital; axes are not
static unsigned char __cdecl ControlIsDigital(const Control* c) {
    int t = c->type;
    return t == 1 || t == 3 || t == 4 || t == 6;
}
static void fp_control_is_digital(Footprint& f, const Control*) { f.pure = true; }
PORT_FN(0x0044d4c0, "ControlIsDigital", ControlIsDigital, fp_control_is_digital)

// control_from_entry: a Control from a table row. A key gets only its scan-code byte (the other three bytes
// of the value are left as they were); a type outside 1..6 gets only the type.
static void __cdecl control_from_entry(Control* c, ControlEntry* e) {
    int t = e->type;
    c->type = t;
    if ((uint32_t)(t - 1) > 5) return;
    if (t == 1) c->key = (uint8_t)e->value;
    else c->value = e->value;
}
static void fp_control_from_entry(Footprint& f, Control* c, ControlEntry*) { f.add(c, sizeof *c, "control"); f.pure = true; }
PORT_FN(0x0044d590, "control_from_entry", control_from_entry, fp_control_from_entry)

// ControlReadAnalog: a control's value from the live inputs, 0..1 (an axis half: 0 to 1, both halves
// -1..1 folded as (1 -+ a) / 2; a key or button 0 or 1). Returned in ST0 unrounded, as the original.
static double __cdecl ControlReadAnalog(const Control* c) {
    const JoyPos* joy = P<JoyPos>(S_JOY);
    const Mouse* mouse = P<Mouse>(S_MOUSE);
    switch (c->type) {
    case 1:
        return ScanDown(c->key) ? 1.0f : 0.0f;
    case 2: {
        uint32_t v = (uint32_t)c->value;
        if (v > 15) return 0.0f;
        // the axis order is X, Y, Rz, Z (DirectInput's X Y Z Rz with the last two swapped): the halves
        // 0..7 are -X +X -Y +Y -Rz +Rz -Z +Z, the folded forms 8..11 and 12..15 one per axis
        static const int axis_of[4] = {0, 1, 3, 2};
        float a = joy->axis[axis_of[v < 8 ? v >> 1 : v & 3]];
        if (v < 8) return max0_nan(v & 1 ? D(a) : -D(a));               // fld a; [fchs]; clamp
        if (v < 12) return (D(1.0f) - D(a)) * D(0.5f);                    // fld 1; fsub a; fmul 0.5
        return (D(a) + D(1.0f)) * D(0.5f);                                // fld a; fadd 1; fmul 0.5
    }
    case 3:
        if ((uint32_t)c->value > 7) return 0.0f;
        return joy->button[c->value] ? 1.0f : 0.0f;
    case 4:                                                               // the hat direction (an int compare)
        return c->value == joy->pov ? 1.0f : 0.0f;
    case 5: {
        uint32_t v = (uint32_t)c->value;
        if (v > 7) return 0.0f;
        float a = (v & 2) ? mouse->y : mouse->x;
        if (v < 4) return max0_nan(v & 1 ? D(a) : -D(a));
        return v & 1 ? (D(a) + D(1.0f)) * D(0.5f) : (D(1.0f) - D(a)) * D(0.5f);
    }
    case 6:
        switch (c->value) {
        case 0: return mouse->button[0] ? 1.0f : 0.0f;
        case 1: return mouse->button[2] ? 1.0f : 0.0f;                   // 1 is the middle button, 2 the right
        case 2: return mouse->button[1] ? 1.0f : 0.0f;
        default: return 0.0f;
        }
    default:
        return 0.0f;
    }
}
static void fp_control_read_analog(Footprint&, const Control*) {}       // reads the inputs, writes nothing
PORT_FN(0x0044ceb0, "ControlReadAnalog", ControlReadAnalog, fp_control_read_analog)

// ControlReadDigital: analog > 0.2 (a NaN reads as not pressed)
static unsigned char __cdecl ControlReadDigital(const Control* c) {
    double v = ControlReadAnalogA(c);
    return v > D((float)0.2f) ? 1 : 0;                                          // fcomp 0.2; test ah,0x41; sete
}
static void fp_control_read_digital(Footprint&, const Control*) {}
PORT_FN(0x0044d470, "ControlReadDigital", ControlReadDigital, fp_control_read_digital)

// ControlReadTrigger: a key's "hit" bit (ScanHit clears it), or a digital read of anything else
static unsigned char __cdecl ControlReadTrigger(const Control* c) {
    int t = c->type;
    if (t == 1) return ScanHit(c->key);
    if (t < 2 || t > 6) return 0;
    return ControlReadDigitalA(c);
}
static void fp_control_read_trigger(Footprint& f, const Control*) { f.add(P<void>(S_KEYBOARD), 256, "keyboard state"); }
PORT_FN(0x0044d490, "ControlReadTrigger", ControlReadTrigger, fp_control_read_trigger)

// ControlUpdate: poll the joystick (when asked: DriverUpdate does every 1/18 s) and the mouse. The mouse
// becomes -1..1 of the half screen, each axis an integer difference divided by an integer on the x87.
static void __cdecl ControlUpdate(unsigned char poll_joystick) {
    if (poll_joystick) {
        JoyPos* joy = P<JoyPos>(S_JOY);
        joy->axis[0] = joy->axis[1] = joy->axis[2] = joy->axis[3] = as_float(0);
        JoyGetPos(joy);
    }
    int x, y, state;
    MousePeek(&x, &y, &state);
    Mouse* m = P<Mouse>(S_MOUSE);
    int half_w = *gxScreenWid / 2;                                       // cdq; sub; sar: toward zero
    int dx = x - half_w;
    int half_h = *gxScreenHit / 2;
    int dy = y - half_h;
    m->x = (float)(D(dx) / D(half_w));                                   // fild; fidiv; fstp
    m->button[0] = (uint8_t)(state & 1);
    m->y = (float)(D(dy) / D(half_h));
    m->button[1] = (uint8_t)((state & 2) >> 1);
    m->button[2] = (uint8_t)((state & 4) >> 2);
}
static void fp_control_update(Footprint& f, unsigned char) { fp_control_inputs(f); }
PORT_FN(0x0044b340, "ControlUpdate", ControlUpdate, fp_control_update)

// ControlDetectInit: read the inputs twice and keep them as the neutral position
static void __cdecl ControlDetectInit(void) {
    Mouse* m = P<Mouse>(S_MOUSE);
    m->x = m->y = as_float(0);
    m->button[2] = m->button[1] = m->button[0] = 0;
    ControlUpdateA(1);
    ControlUpdateA(1);
    memcpy(P<void>(S_NEUTRAL_JOY), P<void>(S_JOY), sizeof(JoyPos));
    memcpy(P<void>(S_NEUTRAL_MOUSE), P<void>(S_MOUSE), sizeof(Mouse));
}
static void fp_control_detect_init(Footprint& f) {
    fp_control_inputs(f);
    f.add(P<void>(S_NEUTRAL_JOY), sizeof(JoyPos), "neutral JoyPos");
    f.add(P<void>(S_NEUTRAL_MOUSE), sizeof(Mouse), "neutral mouse");
}
PORT_FN(0x0044b4b0, "ControlDetectInit", ControlDetectInit, fp_control_detect_init)

// get_neutral_value_analog / _digital: the control's value at the neutral position -- the live JoyPos and
// mouse are swapped for the neutral copies around the read, then put back
static double __cdecl get_neutral_value_analog(const Control* c) {
    if (c->type == 1) return 0.0f;
    JoyPos joy_save;
    Mouse mouse_save;
    memcpy(&joy_save, P<void>(S_JOY), sizeof joy_save);
    memcpy(&mouse_save, P<void>(S_MOUSE), sizeof mouse_save);
    memcpy(P<void>(S_JOY), P<void>(S_NEUTRAL_JOY), sizeof(JoyPos));
    memcpy(P<void>(S_MOUSE), P<void>(S_NEUTRAL_MOUSE), sizeof(Mouse));
    double v = ControlReadAnalogA(c);
    memcpy(P<void>(S_JOY), &joy_save, sizeof joy_save);
    memcpy(P<void>(S_MOUSE), &mouse_save, sizeof mouse_save);
    return v;
}
static void fp_get_neutral_value(Footprint& f, const Control*) { fp_control_inputs(f); }
PORT_FN(0x0044d310, "get_neutral_value_analog", get_neutral_value_analog, fp_get_neutral_value)

static unsigned char __cdecl get_neutral_value_digital(const Control* c) {
    if (c->type == 1) return 0;
    JoyPos joy_save;
    Mouse mouse_save;
    memcpy(&joy_save, P<void>(S_JOY), sizeof joy_save);
    memcpy(&mouse_save, P<void>(S_MOUSE), sizeof mouse_save);
    memcpy(P<void>(S_JOY), P<void>(S_NEUTRAL_JOY), sizeof(JoyPos));
    memcpy(P<void>(S_MOUSE), P<void>(S_NEUTRAL_MOUSE), sizeof(Mouse));
    unsigned char v = ControlReadDigitalA(c);
    memcpy(P<void>(S_JOY), &joy_save, sizeof joy_save);
    memcpy(P<void>(S_MOUSE), &mouse_save, sizeof mouse_save);
    return v;
}
PORT_FN(0x0044d3c0, "get_neutral_value_digital", get_neutral_value_digital, fp_get_neutral_value)

// control_init: build the name table once (the dynamic initialiser of a function-local static array, guarded
// by bit 0 of 0x52260c). 83 rows: 32 joystick axis halves, hat directions and buttons, 4 mouse axes, 3
// mouse buttons, 40 letter/digit/punctuation keys (display name = the key's own character, from
// KeyConvertScanKey) and 11 named keys. Every display name that isn't a single key is a translated string
// (an Xlator: translated when its cookie is stale, in table order). Entry 0's name is statically initialised.
namespace {
struct InitRow {
    uint32_t xlator;                  // the display name's Xlator, or 0: a plain key (inline_display)
    uint8_t type;
    uint8_t value;
    uint8_t reserved;
    uint32_t name;                    // the ini string's address
};
static const InitRow k_init_rows[83] = {
    {0x00522858, 2, 8, 0, 0x004eec48}, {0x00522538, 2, 9, 0, 0x004eec50}, {0x005224a8, 2, 10, 0, 0x004eec58},
    {0x00522548, 2, 11, 0, 0x004eec60}, {0x00522560, 2, 12, 0, 0x004eec68}, {0x00522570, 2, 13, 0, 0x004eec70},
    {0x00522580, 2, 14, 0, 0x004eec78}, {0x00522590, 2, 15, 0, 0x004eec80}, {0x005226c8, 2, 0, 0, 0x004eec88},
    {0x00522828, 2, 1, 0, 0x004eec94}, {0x00522798, 2, 2, 0, 0x004eeca0}, {0x005224d8, 2, 3, 0, 0x004eeca8},
    {0x00522778, 2, 4, 0, 0x004eecb4}, {0x00522838, 2, 5, 0, 0x004eecbc}, {0x005227c8, 2, 6, 0, 0x004eecc8},
    {0x00522628, 2, 7, 0, 0x004eecd0}, {0x00522648, 3, 0, 0, 0x004eecd8}, {0x00522658, 3, 1, 0, 0x004eece4},
    {0x00522668, 3, 2, 0, 0x004eecf0}, {0x00522678, 3, 3, 0, 0x004eecfc}, {0x00522688, 3, 4, 0, 0x004eed08},
    {0x00522698, 3, 5, 0, 0x004eed14}, {0x005226a8, 3, 6, 0, 0x004eed20}, {0x005226b8, 3, 7, 0, 0x004eed2c},
    {0x00522610, 4, 4, 0, 0x004eed38}, {0x005227a8, 4, 1, 0, 0x004eed44}, {0x005224b8, 4, 2, 0, 0x004eed50},
    {0x00522720, 4, 3, 0, 0x004eed5c}, {0x00522788, 5, 2, 0, 0x004eed68}, {0x005227e8, 5, 3, 0, 0x004eed70},
    {0x005224c8, 5, 0, 0, 0x004eed78}, {0x005225a0, 5, 1, 0, 0x004eed80},
    // the plain keys: A B C D E F G H I J K L M N O P Q R S T U V W X Y Z 6 7 8 9 0 [ ] ; ' , . / - =
    {0, 1, 0x1e, 0, 0x004eed88}, {0, 1, 0x30, 0, 0x004eed90}, {0, 1, 0x2e, 0, 0x004eed98}, {0, 1, 0x20, 0, 0x004eeda0},
    {0, 1, 0x12, 0, 0x004eeda8}, {0, 1, 0x21, 0, 0x004eedb0}, {0, 1, 0x22, 0, 0x004eedb8}, {0, 1, 0x23, 0, 0x004eedc0},
    {0, 1, 0x17, 0, 0x004eedc8}, {0, 1, 0x24, 0, 0x004eedd0}, {0, 1, 0x25, 0, 0x004eedd8}, {0, 1, 0x26, 0, 0x004eede0},
    {0, 1, 0x32, 0, 0x004eede8}, {0, 1, 0x31, 0, 0x004eedf0}, {0, 1, 0x18, 0, 0x004eedf8}, {0, 1, 0x19, 0, 0x004eee00},
    {0, 1, 0x10, 0, 0x004eee08}, {0, 1, 0x13, 0, 0x004eee10}, {0, 1, 0x1f, 0, 0x004eee18}, {0, 1, 0x14, 0, 0x004eee20},
    {0, 1, 0x16, 0, 0x004eee28}, {0, 1, 0x2f, 0, 0x004eee30}, {0, 1, 0x11, 0, 0x004eee38}, {0, 1, 0x2d, 0, 0x004eee40},
    {0, 1, 0x15, 0, 0x004eee48}, {0, 1, 0x2c, 0, 0x004eee50}, {0, 1, 0x07, 0, 0x004eee58}, {0, 1, 0x08, 0, 0x004eee60},
    {0, 1, 0x09, 0, 0x004eee68}, {0, 1, 0x0a, 0, 0x004eee70}, {0, 1, 0x0b, 0, 0x004eee78}, {0, 1, 0x1a, 0, 0x004eee80},
    {0, 1, 0x1b, 0, 0x004eee90}, {0, 1, 0x27, 0, 0x004eeea0}, {0, 1, 0x28, 0, 0x004eeeb0}, {0, 1, 0x33, 0, 0x004eeebc},
    {0, 1, 0x34, 0, 0x004eeec8}, {0, 1, 0x35, 0, 0x004eeed4}, {0, 1, 0x0c, 0, 0x004eeee0}, {0, 1, 0x0d, 0, 0x004eeeec},
    // the named keys: SHIFT CTRL RSHIFT RCTRL INS DEL SPACE UP DOWN LEFT RIGHT
    {0x00522848, 1, 0xfd, 0, 0x004eeef8}, {0x005224f8, 1, 0xfe, 0, 0x004eef04}, {0x005227d8, 1, 0x36, 0, 0x004eef10},
    {0x00522710, 1, 0x9d, 0, 0x004eef1c}, {0x00522600, 1, 0xd2, 0, 0x004eef28}, {0x005224e8, 1, 0xd3, 0, 0x004eef30},
    {0x005225f0, 1, 0x39, 1, 0x004eef38}, {0x00522518, 1, 0xc8, 0, 0x004eef44}, {0x00522528, 1, 0xd0, 0, 0x004eef4c},
    {0x005226e0, 1, 0xcb, 0, 0x004eef58}, {0x005227b8, 1, 0xcd, 0, 0x004eef64},
};
}  // namespace

static void __cdecl control_init(void) {
    uint8_t& guard = *P<uint8_t>(S_INIT_GUARD);
    ControlEntry* table = P<ControlEntry>(TABLE);
    bool build = !(guard & 1);                                        // build once...
    if (build) guard |= 1;
    for (int i = 0; build && i < 83; i++) {
        const InitRow& r = k_init_rows[i];
        ControlEntry& e = table[i];
        if (r.xlator) {
            Xlator* x = P<Xlator>(r.xlator);
            if (x->cookie != *P<uint32_t>(S_XLATOR_COOKIE)) Xlator_xlate(x, 0);
            e.display = x->text;
            e.type = r.type;
            e.value = r.value;
            memset(e.inline_display, 0, 4);
            if (r.reserved) e.reserved = 1;                       // Key_SPACE: a byte store, the padding untouched
            else memset(&e.reserved, 0, 4);
        } else {
            e.display = 0;
            e.type = 1;
            e.value = r.value;
            e.inline_display[0] = KeyConvertScanKey(r.value);
            e.inline_display[1] = e.inline_display[2] = e.inline_display[3] = 0;
            memset(&e.reserved, 0, 4);
        }
        if (i) e.name = (const char*)(uintptr_t)r.name;           // entry 0's is in the image
    }
    *P<ControlEntry*>(S_ENTRIES) = table;                             // ...but point at it every call
    *P<int32_t>(S_NUM_ENTRIES) = 83;
}
static void fp_control_init(Footprint& f) { fp_control_table(f); }
PORT_FN(0x0044b620, "control_init", control_init, fp_control_init)

// find_control_entry: the table row with this type and value (a key: its scan-code byte), or 0
static const ControlEntry* __cdecl find_control_entry(const Control* c) {
    control_initA();
    int n = *P<int32_t>(S_NUM_ENTRIES);
    ControlEntry* table = *P<ControlEntry*>(S_ENTRIES);
    int t = c->type;
    for (int i = 0; i < n; i++) {
        const ControlEntry& e = table[i];
        if (e.type != t) continue;
        if ((uint32_t)(t - 1) > 5) continue;
        if (t == 1 ? (uint8_t)e.value == c->key : e.value == c->value) return &table[i];
    }
    return 0;
}
static void fp_find_control_entry(Footprint& f, const Control*) { fp_control_table(f); }
PORT_FN(0x0044d5f0, "find_control_entry", find_control_entry, fp_find_control_entry)

// ControlFromInternalString: the row whose ini name matches (case-insensitively), into c; no match: nothing
static void __cdecl ControlFromInternalString(Control* c, const char* s) {
    control_initA();
    int n = *P<int32_t>(S_NUM_ENTRIES);
    for (int i = 0; i < n; i++) {
        ControlEntry* table = *P<ControlEntry*>(S_ENTRIES);
        if (g_stricmp(table[i].name, s) == 0) {
            control_from_entryA(c, &(*P<ControlEntry*>(S_ENTRIES))[i]);
            return;
        }
    }
}
static void fp_control_from_internal_string(Footprint& f, Control* c, const char*) { f.add(c, sizeof *c, "control"); fp_control_table(f); }
PORT_FN(0x0044d530, "ControlFromInternalString", ControlFromInternalString, fp_control_from_internal_string)

// ControlToInternalString: the ini name into buf[n] (strncpy, always terminated); unknown: ""
static void __cdecl ControlToInternalString(char* buf, int n, const Control* c) {
    const ControlEntry* e = find_control_entryA(c);
    if (e) {
        g_strncpy(buf, e->name, (size_t)n);
        buf[n - 1] = 0;
    } else {
        buf[0] = 0;
        buf[n - 1] = 0;
    }
}
static void fp_control_to_string(Footprint& f, char* buf, int n, const Control*) {
    f.add(buf, (uint32_t)n, "string");
    fp_control_table(f);
}
PORT_FN(0x0044d4e0, "ControlToInternalString", ControlToInternalString, fp_control_to_string)

// ControlToDisplayString: the display name into buf[n]; unknown: the translated "unmapped" text
static void __cdecl ControlToDisplayString(char* buf, int n, const Control* c) {
    const ControlEntry* e = find_control_entryA(c);
    if (e) {
        const char* s = e->display ? e->display : e->inline_display;
        g_strncpy(buf, s, (size_t)n);
        buf[n - 1] = 0;
        return;
    }
    Xlator* x = P<Xlator>(S_XL_UNKNOWN);
    if (x->cookie != *P<uint32_t>(S_XLATOR_COOKIE)) Xlator_xlate(x, 0);
    g_strncpy(buf, x->text, (size_t)n);
    buf[n - 1] = 0;
}
PORT_FN(0x0044ce40, "ControlToDisplayString", ControlToDisplayString, fp_control_to_string)

// ControlGet: the control named `key` in the CONTROL section of the options (OptionsGet creates a missing
// item, so this is checked by replays)
static void __cdecl ControlGet(Control* c, const char* key) {
    char buf[32];
    buf[0] = *P<const char>(S_EMPTY_STRING);                        // char buf[32] = "" the 1998 way
    memset(buf + 1, 0, 31);
    OptionsGetS(*P<const char*>(0x004dca80), key, buf, 32);
    ControlFromInternalStringA(c, buf);
}
static void fp_control_get(Footprint& f, Control* c, const char*) {
    f.add(c, sizeof *c, "control");
    fp_control_table(f);
    f.replay_only = "OptionsGet creates a missing option";
}
PORT_FN(0x0044b430, "ControlGet", ControlGet, fp_control_get)

// ControlSet: store the control's ini name under `key`
static void __cdecl ControlSet(const Control* c, const char* key) {
    char buf[32];
    ControlToInternalStringA(buf, 32, c);
    OptionsSetS(*P<const char*>(0x004dca80), key, buf);
}
static void fp_control_set(Footprint& f, const Control*, const char*) {
    fp_control_table(f);
    f.replay_only = "OptionsSet writes the options";
}
PORT_FN(0x0044b480, "ControlSet", ControlSet, fp_control_set)

// ControlDetect: the first offered row whose input is active and differs from its neutral value -- a
// digital one pressed and not pressed at neutral; an analog one over 0.5 while its neutral is within 0.25.
// The original's local Control keeps three bytes of stack contents above a key's scan code and copies them
// to `out`; here they're zero, so the footprint covers the type and the scan code only (a key's other bytes
// are never read).
static unsigned char __cdecl ControlDetect(Control* out) {
    control_initA();
    int n = *P<int32_t>(S_NUM_ENTRIES);
    for (int i = 0; i < n; i++) {
        ControlEntry* e = &(*P<ControlEntry*>(S_ENTRIES))[i];
        if (e->reserved) continue;
        Control c = {0, {0}};
        control_from_entryA(&c, e);
        if (c.type == 5 || c.type == 6) continue;                   // the mouse can't be mapped
        if (ControlIsDigitalA(&c)) {
            if (!ControlReadDigitalA(&c)) continue;
            if (get_neutral_value_digitalA(&c)) continue;
            *out = c;
            return 1;
        }
        float v = (float)ControlReadAnalogA(&c);
        double neutral = get_neutral_value_analogA(&c);
        if (fabs(neutral) >= 0.25) continue;                         // fabs; fcomp qword 0.25; test ah,1; je: a NaN passes
        if ((int32_t)bits(v) <= 0x3f000000) continue;                // v > 0.5, on the bits
        *out = c;
        return 1;
    }
    return 0;
}
static void fp_control_detect(Footprint& f, Control* out) {
    f.add(out, 5, "control (type and key)");
    fp_control_table(f);
}
PORT_FN(0x0044b510, "ControlDetect", ControlDetect, fp_control_detect)

// =============================================================================================================
// driver.obj
// =============================================================================================================

// DriverIs*Digital: the game_pad option makes everything digital; else the mapped control's type decides
static unsigned char __cdecl DriverIsSteeringDigital(void) {
    if (*P<uint8_t>(S_GAME_PAD)) return 1;
    if (ControlIsDigitalA(P<Control>(S_CTL_STEER_LEFT))) return 1;
    if (ControlIsDigitalA(P<Control>(S_CTL_STEER_RIGHT))) return 1;
    return 0;
}
static void fp_none(Footprint&) {}
PORT_FN(0x00441f60, "DriverIsSteeringDigital", DriverIsSteeringDigital, fp_none)

static unsigned char __cdecl DriverIsBrakeDigital(void) {
    if (*P<uint8_t>(S_GAME_PAD)) return 1;
    return ControlIsDigitalA(P<Control>(S_CTL_BRAKE)) ? 1 : 0;
}
PORT_FN(0x00441fa0, "DriverIsBrakeDigital", DriverIsBrakeDigital, fp_none)

static unsigned char __cdecl DriverIsThrottleDigital(void) {
    if (*P<uint8_t>(S_GAME_PAD)) return 1;
    return ControlIsDigitalA(P<Control>(S_CTL_THROTTLE)) ? 1 : 0;
}
PORT_FN(0x00441fc0, "DriverIsThrottleDigital", DriverIsThrottleDigital, fp_none)

// DriverSetForce: the car's feedback terms, kept for the next JoySetForce (bit copies)
static void __cdecl DriverSetForce(uint32_t a, uint32_t b, uint32_t c) {
    *P<uint32_t>(S_FORCE_A) = a;
    *P<uint32_t>(S_FORCE_B) = b;
    *P<uint32_t>(S_FORCE_C) = c;
}
static void fp_driver_set_force(Footprint& f, uint32_t, uint32_t, uint32_t) {
    f.add(P<void>(S_FORCE_A), 8, "force a, b");
    f.add(P<void>(S_FORCE_C), 4, "force c");
}
PORT_FN(0x00441db0, "DriverSetForce", DriverSetForce, fp_driver_set_force)

// DriverSetNumGears: the car's gear count; the selected gear is pulled down to it
static void __cdecl DriverSetNumGears(int n) {
    *P<int32_t>(S_NUM_GEARS) = n;
    if (n - 1 < *P<int32_t>(S_GEAR)) *P<int32_t>(S_GEAR) = n - 1;
}
static void fp_driver_set_num_gears(Footprint& f, int) {
    f.add(P<void>(S_NUM_GEARS), 4, "num_gears");
    f.add(P<void>(S_GEAR), 4, "gear");
}
PORT_FN(0x00441dd0, "DriverSetNumGears", DriverSetNumGears, fp_driver_set_num_gears)

static void __cdecl DriverSetGear(int g) { *P<int32_t>(S_GEAR) = g; }
static void fp_driver_set_gear(Footprint& f, int) { f.add(P<void>(S_GEAR), 4, "gear"); }
PORT_FN(0x00441ee0, "DriverSetGear", DriverSetGear, fp_driver_set_gear)

// DriverEnd: force feedback off
static void __cdecl DriverEnd(void) { JoyEnableForceFeedback(0); }
PORT_FN(0x00441550, "DriverEnd", DriverEnd, fp_none)

// DriverRefresh: the option scalings and the 15 control mappings from the options
static void __cdecl DriverRefresh(void) {
    *P<float>(S_STEER_SENS) = 1.0f;
    *P<float>(S_THROTTLE_SENS) = 1.0f;
    *P<float>(S_BRAKE_SENS) = 1.0f;
    *P<float>(S_STEER_SPEED) = 1.0f;
    *P<float>(S_THROTTLE_SPEED) = 1.0f;
    *P<float>(S_BRAKE_SPEED) = 1.0f;
    *P<float>(S_STEER_RANGE) = 1.0f;
    *P<float>(S_THROTTLE_RANGE) = 1.0f;
    *P<uint8_t>(S_GAME_PAD) = 0;
    *P<float>(S_BRAKE_RANGE) = 1.0f;
    const char* section = *P<const char*>(0x004dc3ec);                // "CONTROL"
    OptionsGetF(section, (const char*)0x004edb60, P<float>(S_STEER_SENS));          // steer_sensitivity
    OptionsGetF(section, (const char*)0x004edb74, P<float>(S_THROTTLE_SENS));       // throttle_sensitivity
    OptionsGetF(section, (const char*)0x004edb8c, P<float>(S_BRAKE_SENS));          // brake_sensitivity
    OptionsGetF(section, (const char*)0x004edba0, P<float>(S_STEER_RANGE));         // steer_range
    OptionsGetF(section, (const char*)0x004edbac, P<float>(S_THROTTLE_RANGE));      // throttle_range
    OptionsGetF(section, (const char*)0x004edbbc, P<float>(S_BRAKE_RANGE));         // brake_range
    OptionsGetF(section, (const char*)0x004edbc8, P<float>(S_STEER_SPEED));         // steer_speed
    OptionsGetF(section, (const char*)0x004edbd4, P<float>(S_THROTTLE_SPEED));      // throttle_speed
    OptionsGetF(section, (const char*)0x004edbe4, P<float>(S_BRAKE_SPEED));         // brake_speed
    OptionsGetB(section, (const char*)0x004edbf0, P<unsigned char>(S_NONLINEAR));   // nonlinear_steering
    OptionsGetB(section, (const char*)0x004edc04, P<unsigned char>(S_FORCE_FEEDBACK)); // force_feedback
    OptionsGetB(section, (const char*)0x004edc14, P<unsigned char>(S_GAME_PAD));    // game_pad
    if (DriverIsSteeringDigitalA()) *P<uint8_t>(S_NONLINEAR) = 0;
    JoyEnableForceFeedback(*P<uint8_t>(S_FORCE_FEEDBACK));
    ControlGetA(P<Control>(S_CTL_STEER_LEFT), (const char*)0x004edc20);   // steer_left
    ControlGetA(P<Control>(S_CTL_STEER_RIGHT), (const char*)0x004edc2c);  // steer_right
    ControlGetA(P<Control>(S_CTL_THROTTLE), (const char*)0x004edc38);     // throttle
    ControlGetA(P<Control>(S_CTL_BRAKE), (const char*)0x004edc44);        // braking
    ControlGetA(P<Control>(S_CTL_EBRAKE), (const char*)0x004edc4c);       // e_brake
    ControlGetA(P<Control>(S_CTL_REVERSE), (const char*)0x004edc54);      // reverse
    ControlGetA(P<Control>(S_CTL_CLUTCH), (const char*)0x004edc5c);       // clutch
    ControlGetA(P<Control>(S_CTL_UPSHIFT), (const char*)0x004edc64);      // upshift
    ControlGetA(P<Control>(S_CTL_DOWNSHIFT), (const char*)0x004edc6c);    // downshift
    ControlGetA(P<Control>(S_CTL_HELP), (const char*)0x004edc78);         // help
    ControlGetA(P<Control>(S_CTL_AIRLIFT), (const char*)0x004edc80);      // airlift
    ControlGetA(P<Control>(S_CTL_HORN), (const char*)0x004edc88);         // horn
    ControlGetA(P<Control>(S_CTL_LOOK_LEFT), (const char*)0x004edc90);    // look_left
    ControlGetA(P<Control>(S_CTL_LOOK_RIGHT), (const char*)0x004edc9c);   // look_right
    ControlGetA(P<Control>(S_CTL_LOOK_BACK), (const char*)0x004edca8);    // look_back
}
static void fp_driver_refresh(Footprint& f) {
    fp_driver_statics(f);
    fp_control_table(f);
    f.replay_only = "OptionsGet creates a missing option";
}
PORT_FN(0x00441560, "DriverRefresh", DriverRefresh, fp_driver_refresh)

// DriverBegin: everything to rest, then DriverRefresh; always succeeds
static unsigned char __cdecl DriverBegin(void) {
    *P<uint32_t>(S_FF_TIMER) = 0;
    *P<uint8_t>(S_HELP) = 0;
    *P<uint8_t>(S_FORCE_FEEDBACK) = 0;
    *P<uint8_t>(S_NONLINEAR) = 0;
    *P<float>(S_STEER_SENS) = 1.0f;
    *P<float>(S_STEER_SPEED) = 1.0f;
    *P<float>(S_STEER_RANGE) = 1.0f;
    *P<float>(S_THROTTLE_SENS) = 1.0f;
    *P<float>(S_THROTTLE_SPEED) = 1.0f;
    *P<float>(S_THROTTLE_RANGE) = 1.0f;
    *P<float>(S_BRAKE_SENS) = 1.0f;
    *P<float>(S_BRAKE_SPEED) = 1.0f;
    *P<float>(S_BRAKE_RANGE) = 1.0f;
    *P<int32_t>(S_GEAR) = 0;
    *P<int32_t>(S_NUM_GEARS) = 0;
    *P<uint32_t>(S_STEERING) = 0;
    *P<uint32_t>(S_BRAKING) = 0;
    *P<uint32_t>(S_THROTTLE) = 0;
    *P<uint32_t>(S_CLUTCH) = 0;
    *P<uint32_t>(S_EBRAKE) = 0;
    *P<uint8_t>(S_HORN) = 0;
    *P<uint8_t>(S_AIRLIFT) = 0;
    *P<uint8_t>(S_SHIFT_LATCH) = 0;
    *P<uint8_t>(S_REVERSE) = 0;
    *P<uint32_t>(S_FORCE_C) = 0;
    *P<uint32_t>(S_FORCE_B) = 0;
    *P<uint32_t>(S_FORCE_A) = 0;
    *P<uint32_t>(S_LOOK_SIDE) = 0;
    *P<uint8_t>(S_LOOK_BACK) = 0;
    *P<uint8_t>(S_GAME_PAD) = 0;
    DriverRefreshA();
    return 1;
}
PORT_FN(0x00441490, "DriverBegin", DriverBegin, fp_driver_refresh)

// DriverUpdate(dt): the mapped controls into the driver's outputs.
//   * every 1/18 s the joystick is polled and JoySetForce gets the car's feedback terms;
//   * an analog axis pair is right - left, with a dead band of 1 - steer_sensitivity, times steer_range;
//     digital steering ramps at steer_speed x 2.5 toward centre and x 5 under a key, clamped to -1..1;
//   * an analog pedal is reading x range + sensitivity - 1, clamped to 0..1; digital ramps down at speed x 6
//     and up at x 12;
//   * the clutch key ramps up at 10/s and down at 5/s; shifts step the gear once per key press.
static void __cdecl DriverUpdate(float dt) {
    // the force-feedback timer
    double t = D(*P<float>(S_FF_TIMER)) + D(dt);
    *P<float>(S_FF_TIMER) = (float)t;
    unsigned char tick = 0;
    if (t >= D(as_float(0x3d638e39))) {                              // 1/18 s: !(t < k || unordered)
        *P<uint32_t>(S_FF_TIMER) = 0;
        tick = 1;
    }
    ControlUpdateA(tick);

    // steering
    float& steering = *P<float>(S_STEERING);
    uint32_t& steering_bits = *P<uint32_t>(S_STEERING);
    const Control* left = P<Control>(S_CTL_STEER_LEFT);
    const Control* right = P<Control>(S_CTL_STEER_RIGHT);
    if (!*P<uint8_t>(S_GAME_PAD) && !ControlIsDigitalA(left) && !ControlIsDigitalA(right)) {
        float r = (float)ControlReadAnalogA(right);
        double l = ControlReadAnalogA(left);
        steering = (float)(D(r) - l);
        float dead = (float)(D(1.0f) - D(*P<float>(S_STEER_SENS)));
        bool positive = (int32_t)steering_bits > 0;
        double v = (D(dead) + D(1.0f)) * D(steering);
        if (positive) v = max0_nan(v - D(dead));
        else v = min0(v + D(dead));
        steering = (float)v;
        double s = D(*P<float>(S_STEER_RANGE)) * D(steering);
        steering = (float)s;
        if (!(s >= D(-1.0f))) steering_bits = 0xbf800000u;           // s < -1, or unordered
        if ((int32_t)steering_bits > 0x3f800000) steering_bits = 0x3f800000u;
    } else {
        double rate_r = D(*P<float>(S_STEER_SPEED)) * D(2.5f);
        bool positive = (int32_t)steering_bits > 0;
        float rate = (float)rate_r;
        float rate2 = (float)(rate_r * D(2.0f));
        if (positive) steering = (float)max0_nan(D(steering) - D(rate) * D(dt));
        if (steering_bits > 0x80000000u) steering = (float)min0(D(rate) * D(dt) + D(steering));
        if (ControlReadDigitalA(left)) steering = (float)(D(steering) - D(rate2) * D(dt));
        if (ControlReadDigitalA(right)) steering = (float)(D(rate2) * D(dt) + D(steering));
        if ((int32_t)steering_bits > 0x3f800000) steering_bits = 0x3f800000u;
        if (steering_bits > 0xbf800000u) steering_bits = 0xbf800000u;
    }

    // throttle
    float& throttle = *P<float>(S_THROTTLE);
    uint32_t& throttle_bits = *P<uint32_t>(S_THROTTLE);
    const Control* gas = P<Control>(S_CTL_THROTTLE);
    if (!*P<uint8_t>(S_GAME_PAD) && !ControlIsDigitalA(gas)) {
        double v = ControlReadAnalogA(gas) * D(*P<float>(S_THROTTLE_RANGE));
        v = v + D(*P<float>(S_THROTTLE_SENS));
        v = v - D(1.0f);
        throttle = (float)v;
        if (v > D(1.0f)) throttle_bits = 0x3f800000u;                 // !(v <= 1 || unordered)
        if (throttle_bits > 0x80000000u) throttle_bits = 0;
    } else {
        float rate12 = (float)(D(*P<float>(S_THROTTLE_SPEED)) * D(12.0f));
        bool positive = (int32_t)throttle_bits > 0;
        float rate6 = (float)(D(*P<float>(S_THROTTLE_SPEED)) * D(6.0f));
        if (positive) throttle = (float)max0_nan(D(throttle) - D(rate6) * D(dt));
        if (ControlReadDigitalA(gas)) throttle = (float)(D(rate12) * D(dt) + D(throttle));
        if ((int32_t)throttle_bits > 0x3f800000) throttle_bits = 0x3f800000u;
    }

    // braking
    float& braking = *P<float>(S_BRAKING);
    uint32_t& braking_bits = *P<uint32_t>(S_BRAKING);
    const Control* brake = P<Control>(S_CTL_BRAKE);
    if (!*P<uint8_t>(S_GAME_PAD) && !ControlIsDigitalA(brake)) {
        double v = ControlReadAnalogA(brake) * D(*P<float>(S_BRAKE_RANGE));
        v = v + D(*P<float>(S_BRAKE_SENS));
        v = v - D(1.0f);
        braking = (float)v;
        if (v > D(1.0f)) braking_bits = 0x3f800000u;
        if (braking_bits > 0x80000000u) braking_bits = 0;
    } else {
        float rate12 = (float)(D(*P<float>(S_BRAKE_SPEED)) * D(12.0f));
        bool positive = (int32_t)braking_bits > 0;
        float rate6 = (float)(D(*P<float>(S_BRAKE_SPEED)) * D(6.0f));
        if (positive) braking = (float)max0_nan(D(braking) - D(rate6) * D(dt));
        if (ControlReadDigitalA(brake)) braking = (float)(D(rate12) * D(dt) + D(braking));
        if ((int32_t)braking_bits > 0x3f800000) braking_bits = 0x3f800000u;
    }

    *P<float>(S_EBRAKE) = (float)ControlReadAnalogA(P<Control>(S_CTL_EBRAKE));
    *P<uint8_t>(S_REVERSE) = ControlReadDigitalA(P<Control>(S_CTL_REVERSE));

    // clutch
    float& clutch = *P<float>(S_CLUTCH);
    uint32_t& clutch_bits = *P<uint32_t>(S_CLUTCH);
    const Control* clutch_ctl = P<Control>(S_CTL_CLUTCH);
    if (ControlIsDigitalA(clutch_ctl)) {
        if (ControlReadDigitalA(clutch_ctl)) clutch = (float)(D(dt) * D(10.0f) + D(clutch));
        else if ((int32_t)clutch_bits > 0) clutch = (float)max0_nan(D(dt) * D(-5.0f) + D(clutch));
        if ((int32_t)clutch_bits > 0x3f800000) clutch_bits = 0x3f800000u;
    } else {
        clutch = (float)ControlReadAnalogA(clutch_ctl);
    }

    *P<uint8_t>(S_HELP) = ControlReadDigitalA(P<Control>(S_CTL_HELP));
    *P<uint8_t>(S_HORN) = ControlReadDigitalA(P<Control>(S_CTL_HORN));
    *P<uint8_t>(S_AIRLIFT) = ControlReadDigitalA(P<Control>(S_CTL_AIRLIFT));
    {
        float l = (float)ControlReadAnalogA(P<Control>(S_CTL_LOOK_LEFT));
        double r = ControlReadAnalogA(P<Control>(S_CTL_LOOK_RIGHT));
        *P<float>(S_LOOK_SIDE) = (float)(r - D(l));
    }
    *P<uint8_t>(S_LOOK_BACK) = ControlReadDigitalA(P<Control>(S_CTL_LOOK_BACK));
    {
        float down = (float)ControlReadAnalogA(P<Control>(S_CTL_DOWNSHIFT));
        double up = ControlReadAnalogA(P<Control>(S_CTL_UPSHIFT));
        *P<float>(S_PITCH) = (float)(D(down) - up);
    }

    // gears: one step per press
    uint8_t& latch = *P<uint8_t>(S_SHIFT_LATCH);
    int32_t& gear = *P<int32_t>(S_GEAR);
    if (ControlReadDigitalA(P<Control>(S_CTL_UPSHIFT))) {
        if (!latch) {
            if (*P<int32_t>(S_NUM_GEARS) > gear) gear = gear + 1;
            latch = 1;
        }
    } else if (ControlReadDigitalA(P<Control>(S_CTL_DOWNSHIFT))) {
        if (!latch) {
            if (gear > -1) gear = gear - 1;
            latch = 1;
        }
    } else {
        latch = 0;
    }

    if (tick) JoySetForce(*P<uint32_t>(S_FORCE_A), *P<uint32_t>(S_FORCE_B), *P<uint32_t>(S_FORCE_C));
}
// The driver's statics except the three force-feedback values, which DriverUpdate only reads (for JoySetForce):
// they belong to the physics thread, whose PlayCar::Update writes them (DriverSetForce) at any moment of this
// main-thread check. Listed, the check saved and restored them around its passes -- a physics-thread write
// between the snapshot and the compare was reported as DriverUpdate's mismatch (driver.obj statics +0x64 =
// force a), and the restore could undo a physics write in the middle of PlayCar::Update's own check (its
// "driver force a, b" mismatch).
static void fp_driver_update(Footprint& f, float) {
    f.add(P<void>(DRV_BASE), S_FORCE_A - DRV_BASE, "driver.obj statics");
    f.add(P<void>(S_FORCE_B + 4), S_FORCE_C - (S_FORCE_B + 4), "driver.obj statics");
    f.add(P<void>(S_FORCE_C + 4), DRV_BASE + DRV_SIZE - (S_FORCE_C + 4), "driver.obj statics");
    fp_control_inputs(f);
}
PORT_FN(0x004417c0, "DriverUpdate", DriverUpdate, fp_driver_update)

// =============================================================================================================
// localcar.obj
// =============================================================================================================

static void* __fastcall LocalCar_ctor(void* self, Edx, void* data, void* p) {
    Car_Car(self, 0, data, p);
    at<uint32_t>(self, 0) = LOCALCAR_VTABLE;
    void* deity = *g_deity;
    VFN(deity, 0x2c, void, int, void*)(deity, 0, at<int32_t>(self, C_CAR_INDEX), self);
    MultiRegisterLocalCar(self, at<int32_t>(self, C_CAR_INDEX));
    at<uint32_t>(self, L_EC4) = 0;
    at<uint8_t>(self, L_TELEPORTED) = 0;
    return self;
}
static void fp_localcar_ctor(Footprint& f, void* self, Edx, void*, void*) {
    f.add(self, 3784, "LocalCar");
    f.replay_only = "constructs a Car (allocations) and registers it";
}
PORT_FN(0x004444e0, "LocalCar::LocalCar", LocalCar_ctor, fp_localcar_ctor)

static void __fastcall LocalCar_dtor(void* self, Edx) {
    at<uint32_t>(self, 0) = LOCALCAR_VTABLE;
    MultiUnregisterLocalCar(self);
    Car_dtor(self, 0);
}
static void fp_localcar_dtor(Footprint& f, void* self, Edx) {
    f.add(self, 3784, "LocalCar");
    f.replay_only = "destroys a Car (frees)";
}
PORT_FN(0x00444540, "LocalCar::~LocalCar", LocalCar_dtor, fp_localcar_dtor)

// LocalCar::Teleport: Car::Teleport, and the flag the next net packet carries
static void __fastcall LocalCar_Teleport(void* self, Edx, const void* frame, const void* point) {
    Car_Teleport(self, 0, frame, point);
    at<uint8_t>(self, L_TELEPORTED) = 1;
}
static void fp_car(Footprint& f, void* self, Edx, const void*, const void*) { f.object(self, "car"); }
PORT_FN(0x00444560, "LocalCar::Teleport", LocalCar_Teleport, fp_car)

// LocalCar::FillNetPacket: this car's state for the peers -- time, pedals, gear, rpm, horn, the teleport
// flag, a byte per wheel, the position and the orientation as a quaternion from the (renormalised) matrix
static void __fastcall LocalCar_FillNetPacket(void* self, Edx, uint8_t* pkt) {
    at<float>(pkt, 0x04) = PhysicsGetTime();
    at<uint32_t>(pkt, 0x18) = at<uint32_t>(self, C_PERCEIVED_RPM);
    at<uint32_t>(pkt, 0x14) = at<uint32_t>(self, C_GEAR);
    at<uint32_t>(pkt, 0x0c) = at<uint32_t>(self, C_STEERING);
    at<uint32_t>(pkt, 0x08) = at<uint32_t>(self, C_BRAKING);
    at<uint8_t>(pkt, 0x38) = at<uint8_t>(self, C_HORN);
    at<uint32_t>(pkt, 0x10) = at<uint32_t>(self, C_ENGINE_SMOOTHED_THROTTLE);
    at<uint8_t>(pkt, 0x39) = at<uint8_t>(self, L_TELEPORTED);
    for (int i = 0; i < 4; i++) at<uint8_t>(pkt, 0x3a + i) = at<uint8_t>(self, C_WHEELS + i * WHEEL_SIZE + W_FLAG20);
    at<uint32_t>(pkt, 0x1c) = at<uint32_t>(self, C_POS);
    at<uint32_t>(pkt, 0x20) = at<uint32_t>(self, C_POS + 4);
    at<uint32_t>(pkt, 0x24) = at<uint32_t>(self, C_POS + 8);
    float* m = &at<float>(self, C_ROT);
    MatrixNormalize(m);
    float* q = &at<float>(pkt, 0x28);                                   // x y z w
    double tr = D(m[8]) + D(m[4]);
    tr = tr + D(m[0]);
    float trf = (float)tr;                                              // fcom 0; fstp
    if (tr > 0.0) {
        double s = x87_sqrt(D(trf) + D(1.0f));
        q[3] = (float)(D(0.5f) * s);
        s = D(0.5f) / s;
        q[0] = (float)((D(m[5]) - D(m[7])) * s);
        q[1] = (float)((D(m[6]) - D(m[2])) * s);
        q[2] = (float)((D(m[1]) - D(m[3])) * s);
    } else {
        int i = 0;
        if (D(m[4]) > D(m[0])) i = 1;                                   // fcomp; test ah,0x41; jne
        if (!(D(m[4 * i]) >= D(m[8]))) i = 2;                           // fcomp; test ah,1; je
        int j = (i + 1) % 3, k = (j + 1) % 3;
        double s = D(m[4 * k]) + D(m[4 * j]);
        s = D(m[4 * i]) - s;
        s = x87_sqrt(s + D(1.0f));
        q[i] = (float)(D(0.5f) * s);
        s = D(0.5f) / s;
        q[3] = (float)((D(m[3 * j + k]) - D(m[3 * k + j])) * s);
        q[j] = (float)((D(m[3 * i + j]) + D(m[3 * j + i])) * s);
        q[k] = (float)((D(m[3 * i + k]) + D(m[3 * k + i])) * s);
    }
    at<uint8_t>(self, L_TELEPORTED) = 0;
}
static void fp_fill_net_packet(Footprint& f, void* self, Edx, uint8_t* pkt) {
    f.object(self, "car");
    f.add(pkt, 0x3e, "packet");
}
PORT_FN(0x00444580, "LocalCar::FillNetPacket", LocalCar_FillNetPacket, fp_fill_net_packet)

// =============================================================================================================
// playcar.obj
// =============================================================================================================

// the sound objects: SoundDash +0x29..0x2b are the play / stop flags PlayCar::Update sets, +0x30 the pan
// and +0x34 its dirty flag the constructor sets
enum : uint32_t { SND_PLAY = 0x29, SND_STOP_A = 0x2a, SND_STOP_B = 0x2b, SND_PAN = 0x30, SND_PAN_DIRTY = 0x34 };

static void* __fastcall PlayCar_ctor(void* self, Edx, void* data, void* p) {
    LocalCar_LocalCar(self, 0, data, p);
    at<uint32_t>(self, 0) = PLAYCAR_VTABLE;
    int traction = 1, abs_braking = 1;
    at<int32_t>(self, C_REALISM) = PhysicsGetRealism();
    const char* section = *P<const char*>(0x004dc1e8);                  // "CONTROL"
    OptionsGetI(section, (const char*)0x004ed9a0, &traction);           // traction_control
    OptionsGetI(section, (const char*)0x004ed9b4, &abs_braking);        // abs_braking
    Car_SetTractionControl(self, 0, traction);
    Car_SetABSBraking(self, 0, abs_braking);
    at<uint8_t>(self, PC_AUTO_SHIFT) = 1;
    OptionsGetB(section, (const char*)0x004ed9c0, &at<unsigned char>(self, PC_AUTO_SHIFT));    // auto_shifting
    at<uint8_t>(self, PC_AUTO_CLUTCH) = 1;
    OptionsGetB(section, (const char*)0x004ed9d0, &at<unsigned char>(self, PC_AUTO_CLUTCH));   // auto_clutch
    if (at<uint8_t>(self, PC_AUTO_SHIFT)) at<uint8_t>(self, PC_AUTO_CLUTCH) = 1;
    at<uint8_t>(self, C_AUTO_CLUTCH) = at<uint8_t>(self, PC_AUTO_CLUTCH);
    int yaw = 0;
    OptionsGetI(section, (const char*)0x004ed9dc, &yaw);                // yaw_control
    at<int32_t>(self, C_YAW_CONTROL) = yaw;
    at<uint8_t>(self, C_DIGITAL_THROTTLE) = DriverIsThrottleDigitalA();
    if (DriverIsThrottleDigitalA())
        for (int i = 0; i < 4; i++) at<uint8_t>(self, C_WHEELS + i * WHEEL_SIZE + W_DIGITAL_THROTTLE) = 1;
    DriverSetNumGearsA(at<int32_t>(self, C_TRANS_NUM_GEARS));
    AIRegisterPlayCar(self, at<int32_t>(self, C_CAR_INDEX));
    for (int i = 0; i < 4; i++) HackGripBoost();                         // called for nothing, four times
    at<uint32_t>(self, PC_SPOT_FLAGS_PREV) = 0;
    at<uint32_t>(self, PC_SPOT_FLAGS) = 0;
    at<uint8_t>(self, PC_SPOTTER) = 0;
    OptionsGetB(*P<const char*>(0x004dc1e4), (const char*)0x004ed9e8, &at<unsigned char>(self, PC_SPOTTER));  // SOUND spotter
    at<uint32_t>(self, PC_SND_CLEAR) = 0;
    at<uint32_t>(self, PC_SND_BOTH) = 0;
    at<uint32_t>(self, PC_SND_RIGHT) = 0;
    at<uint32_t>(self, PC_SND_LEFT) = 0;
    if (at<uint8_t>(self, PC_SPOTTER)) {
        int idx = at<int32_t>(self, C_CAR_INDEX);
        void* s = SoundDash_Create((const char*)0x004ed9f0, 6, idx);     // cleft.sfx, panned left
        at<void*>(self, PC_SND_LEFT) = s;
        if (s && at<uint32_t>(s, SND_PAN) != 0xbf000000u) {
            at<uint8_t>(s, SND_PAN_DIRTY) = 1;
            at<uint32_t>(s, SND_PAN) = 0xbf000000u;
        }
        s = SoundDash_Create((const char*)0x004ed9fc, 6, at<int32_t>(self, C_CAR_INDEX));   // right
        at<void*>(self, PC_SND_RIGHT) = s;
        if (s && at<uint32_t>(s, SND_PAN) != 0x3f000000u) {
            at<uint8_t>(s, SND_PAN_DIRTY) = 1;
            at<uint32_t>(s, SND_PAN) = 0x3f000000u;
        }
        at<void*>(self, PC_SND_BOTH) = SoundDash_Create((const char*)0x004eda08, 6, at<int32_t>(self, C_CAR_INDEX));
        at<void*>(self, PC_SND_CLEAR) = SoundDash_Create((const char*)0x004eda14, 6, at<int32_t>(self, C_CAR_INDEX));
    }
    at<uint32_t>(self, PC_SPOT_TIME) = 0;
    return self;
}
static void fp_playcar_ctor(Footprint& f, void* self, Edx, void*, void*) {
    f.add(self, 3824, "PlayCar");
    f.add(P<void>(S_NUM_GEARS), 4, "driver num_gears");                 // DriverSetNumGears
    f.add(P<void>(S_GEAR), 4, "driver gear");
    f.replay_only = "constructs a Car, reads options, creates sounds, registers with the AI";
}
PORT_FN(0x0043e710, "PlayCar::PlayCar", PlayCar_ctor, fp_playcar_ctor)

static void __fastcall PlayCar_dtor(void* self, Edx) {
    int idx = at<int32_t>(self, C_CAR_INDEX);
    at<uint32_t>(self, 0) = PLAYCAR_VTABLE;
    AIUnregisterCar(self, idx);
    static const uint32_t sounds[4] = {PC_SND_LEFT, PC_SND_RIGHT, PC_SND_BOTH, PC_SND_CLEAR};
    for (int i = 0; i < 4; i++) {
        void* s = at<void*>(self, sounds[i]);
        if (s) VFN(s, 0, void*, unsigned)(s, 0, 1);                     // the deleting destructor
    }
    LocalCar_dtor_orig(self, 0);
}
static void fp_playcar_dtor(Footprint& f, void* self, Edx) {
    f.add(self, 3824, "PlayCar");
    f.replay_only = "deletes the sounds and the Car";
}
PORT_FN(0x0043e960, "PlayCar::~PlayCar", PlayCar_dtor, fp_playcar_dtor)

static void __fastcall PlayCar_Reset(void* self, Edx) {
    Car_Reset(self, 0);
    DriverSetGearA(0);
}
static void fp_playcar_reset(Footprint& f, void* self, Edx) {
    f.object(self, "car");
    f.add(P<void>(S_GEAR), 4, "driver gear");
}
PORT_FN(0x0043e9d0, "PlayCar::Reset", PlayCar_Reset, fp_playcar_reset)

static void __fastcall PlayCar_Teleport(void* self, Edx, const void* frame, const void* point) {
    LocalCar_Teleport_orig(self, 0, frame, point);
    DriverSetGearA(0);
}
static void fp_playcar_teleport(Footprint& f, void* self, Edx, const void*, const void*) {
    f.object(self, "car");
    f.add(P<void>(S_GEAR), 4, "driver gear");
}
PORT_FN(0x0043e9e0, "PlayCar::Teleport", PlayCar_Teleport, fp_playcar_teleport)

// the forward speed: rotation row 2 (the body's z axis) dot the velocity, in the original's grouping
static inline double forward_speed(void* self) {
    const float* m = &at<float>(self, C_ROT);
    const float* v = &at<float>(self, C_VELOCITY);
    double s = D(m[8]) * D(v[2]);
    s = D(v[1]) * D(m[7]) + s;
    s = D(v[0]) * D(m[6]) + s;
    return s;
}

// PlayCar::Update: the driver's controls on to the car, Car::Update, the telemetry and the spotter
static void __fastcall PlayCar_Update(void* self, Edx) {
    float speed = (float)fabs(forward_speed(self));
    Car_SetSteering(self, 0, DriverGetSteering(speed));
    float boost = HackThrottleBoost();
    Car_SetThrottle(self, 0, (float)(D(DriverGetThrottle()) * D(boost)));
    Car_SetBraking(self, 0, DriverGetBraking());
    Car_SetEBrake(self, 0, DriverGetEBrake());
    if (DriverGetReverse()) {
        Car_SetGear(self, 0, -1);
        float t = DriverGetThrottle();
        Car_SetThrottle(self, 0, D(t) <= D((float)0.3f) ? (float)0.3f : t);         // at least 0.3 in reverse (NaN: t)
    } else if (at<uint8_t>(self, PC_AUTO_SHIFT)) {
        Car_SetGearAuto(self, 0);
    } else {
        int gear = DriverGetGear();
        if (at<uint8_t>(self, PC_AUTO_CLUTCH) || (int32_t)at<uint32_t>(self, C_CLUTCH_ENGAGEMENT) < 0x3f666666)
            Car_SetGear(self, 0, gear);                                // the clutch must be in (< 0.9)
        DriverSetGearA(at<int32_t>(self, C_GEAR));
    }
    Car_SetClutch(self, 0, DriverGetClutch());
    if (DriverGetHelp()) Car_HelpOut(self, 0);
    uint32_t status = (int32_t)at<uint32_t>(self, C_BRAKING) > 0x3dcccccd ? 1 : 0;           // brake > 0.1
    if ((int32_t)at<uint32_t>(self, C_ENGINE_SMOOTHED_THROTTLE) > 0x3ecccccd) status |= 2;  // gas > 0.4
    at<uint32_t>(self, C_STATUS) = status;
    unsigned char horn = DriverGetHorn();
    if (DriverGetAirlift()) {
        float& airlift_time = at<float>(self, PC_AIRLIFT_TIME);
        if ((at<uint32_t>(self, PC_AIRLIFT_TIME) & 0x7fffffff) == 0) {
            void* deity = *g_deity;
            VFN(deity, 0x3c, void, int, void*)(deity, 0, at<int32_t>(self, C_CAR_INDEX), self);
            airlift_time = PhysicsGetTime();
        } else if (D(PhysicsGetTime()) - D(airlift_time) > D((float)0.1f)) {
            at<uint32_t>(self, PC_AIRLIFT_TIME) = 0x4ceb79a3u;          // 1.2345e8: held, no second lift
        }
    } else {
        at<uint32_t>(self, PC_AIRLIFT_TIME) = 0;
    }
    Car_SetHorn(self, 0, horn);
    DriverSetForceA(at<uint32_t>(self, C_STEER_FEEDBACK), at<uint32_t>(self, C_LONG_G), at<uint32_t>(self, C_VERT_G));
    at<float>(self, C_LOOK_SIDE) = DriverGetLookSide();
    unsigned char look_back = DriverGetLookBack();
    bool stalled = at<uint8_t>(self, C_ENGINE_STALLED) != 0;
    at<uint8_t>(self, C_LOOK_BACK) = look_back;
    if (stalled && (int32_t)at<uint32_t>(self, C_CLUTCH_ENGAGEMENT) < 0x3c23d70a)             // clutch in (< 0.01)
        Engine_Crank((uint8_t*)self + C_ENGINE, 0);
    if (at<int32_t>(self, C_RACE_STATE) == 3) {                          // finished: coast to a stop
        Car_SetThrottle(self, 0, as_float(0));
        float b = at<float>(self, C_BRAKING);
        Car_SetBraking(self, 0, D(b) <= D((float)0.2f) ? (float)0.2f : b);
    }
    if (at<uint8_t>(self, C_IS_PLANE)) Car_SetPitch(self, 0, DriverGetPitch());
    Car_Update(self, 0);

    // the telemetry: position and lap from the deity, then speed and g's
    int idx = at<int32_t>(self, C_CAR_INDEX);
    uint32_t* tel = PhysTaskGetTelemetry();
    void* deity = *g_deity;
    tel[0] = VFN(deity, 0x50, uint32_t, int)(deity, 0, idx);
    tel = PhysTaskGetTelemetry();
    deity = *g_deity;
    tel[1] = VFN(deity, 0x48, uint32_t, int)(deity, 0, idx);
    PhysTaskGetTelemetry()[2] = at<uint32_t>(self, C_SPEED);
    PhysTaskGetTelemetry()[3] = at<uint32_t>(self, C_LAT_G);
    PhysTaskGetTelemetry()[4] = at<uint32_t>(self, C_LONG_G);

    // the spotter: every 0.5 s while moving forward (upright, over 3 m/s), which cars sit alongside --
    // within 7 m sideways (over 0.7), between 5 m behind and 4 m ahead; a change plays the matching call
    float now = PhysicsGetTime();
    bool moving = false;
    if ((int32_t)at<uint32_t>(self, C_ROT + 4 * 4) > 0) moving = forward_speed(self) > D(3.0f);
    if (!moving) {
        at<uint32_t>(self, PC_SPOT_FLAGS_PREV) = 0;
        at<uint32_t>(self, PC_SPOT_FLAGS) = 0;
    }
    if (!at<uint8_t>(self, PC_SPOTTER) || !moving) return;
    if (!(D(now) - D(at<float>(self, PC_SPOT_TIME)) > D(0.5f))) return;
    uint32_t& flags = at<uint32_t>(self, PC_SPOT_FLAGS);
    flags = 0;
    const float* m = &at<float>(self, C_ROT);
    const float* pos = &at<float>(self, C_POS);
    for (int i = 0; i < 16; i++) {
        void* c = PhysTaskFindCar(i);
        if (!c || c == self) continue;
        if (!VFN(c, 0x30, unsigned char)(c, 0)) continue;                // IsSolid
        const float* cp = &at<float>(c, C_POS);
        float dx = (float)(D(cp[0]) - D(pos[0]));
        float dy = (float)(D(cp[1]) - D(pos[1]));
        double dzr = D(cp[2]) - D(pos[2]);
        float dz = (float)dzr;
        double lon = dzr * D(m[8]);
        lon = D(m[7]) * D(dy) + lon;
        lon = D(m[6]) * D(dx) + lon;
        float lonf = (float)lon;
        if (!(lon > D(-5.0f))) continue;
        if ((int32_t)bits(lonf) >= 0x40800000) continue;
        double lat = D(m[1]) * D(dy);
        lat = D(m[0]) * D(dx) + lat;
        lat = D(m[2]) * D(dz) + lat;
        uint32_t lb = bits((float)lat);
        if (lat > 0.0) {
            if ((int32_t)lb > 0x3f333333 && (int32_t)lb < 0x40e00000) flags |= 2;   // on the right
        } else {
            if (lb > 0xbf333333u && lb < 0xc0e00000u) flags |= 1;                    // on the left
        }
    }
    uint32_t f = flags;
    if (f == at<uint32_t>(self, PC_SPOT_FLAGS_PREV)) return;
    at<uint32_t>(self, PC_SPOT_FLAGS_PREV) = f;
    at<float>(self, PC_SPOT_TIME) = now;
    void* s = at<void*>(self, f == 0 ? PC_SND_CLEAR : f == 1 ? PC_SND_LEFT : f == 2 ? PC_SND_RIGHT : PC_SND_BOTH);
    if (!s) return;
    at<uint8_t>(s, SND_PLAY) = 1;
    at<uint8_t>(s, SND_STOP_B) = 0;
    at<uint8_t>(s, SND_STOP_A) = 0;
}
// Car::Update's reach, as phys_car_update.cpp's fp_car_update lists it (it runs inside, as its original, in a
// check): the body's collision volumes and group spheres; the five live models' vertices and model_info
// +0x10/+0x15 (a dent: the wheels' hub vertices point into the LOD-0 mesh, so they are physics state); the splash
// Sound3D; a plane's wings; and the deity -- RaceDeity::UpdateCar advances the car's record -- with the car's
// CenterLine. Replay only where Car::Update's is (out of bounds, the damage reset, a deity that isn't a
// RaceDeity), and when the airlift fires (the deity's TeleportToLine). Before, only the car was listed: the
// rewrite pass's UpdateCar started from the record the original pass had already advanced, so the telemetry
// taken from it (GetDLong -> PhysicsTelemetry, phystask.obj 0x520c58) came out a little different, and the
// dents leaked as in Car::Update's own checks. (The horn-ball hack's thrown ball -- Car::SetHorn's footprint -- is
// not listed: whether the horn rises is an input.)
static void fp_playcar_car_update(Footprint& f, void* self) {
    const uint8_t* c = (const uint8_t*)self;
    const int32_t nvol = *(const int32_t*)(c + 0x24);                     // PhobRoot volumes[4] (+0x14), count
    for (int i = 0; i < nvol && i < 4; i++) {
        uint8_t* v = ((uint8_t* const*)(c + 0x14))[i];
        if (!v) continue;
        f.object(v, "volume");
        if (*(const uint32_t*)(v + 0x14) == 0x47525550u) {                // 'GRUP': a SphereGroupVolume's spheres
            const int n = *(const int32_t*)(v + 0x4c);
            for (int k = 0; k < n && k < 12; k++)
                if (void* s = ((void* const*)(v + 0x1c))[k]) f.object(s, "body sphere");
        }
    }
    const int32_t* live_models = (const int32_t*)(c + 0x4c4);             // Car +0x4c4 [5]
    for (int i = 0; i < 5; i++) {
        uint8_t* mi = (uint8_t*)(uintptr_t)live_models[i];
        if (!mi) continue;
        f.add(mi + 0x10, 8, "model_info");
        const uint8_t* info = *(const uint8_t* const*)(mi + 0x10);         // mrModelInfo: count, vertices (32 bytes)
        if (!info) continue;
        const int32_t n = *(const int32_t*)info;
        void* verts = *(void* const*)(info + 4);
        if (verts && n > 0) f.add(verts, (uint32_t)n * 32, "model verts");
    }
    if (uint8_t* snd = *(uint8_t**)0x00521f7c) f.add(snd + 8, 0x25, "splash Sound3D");
    if (at<uint8_t>(self, C_IS_PLANE)) {
        static const char* const wings[4] = {"wing_left", "wing_right", "elevator", "rudder"};
        for (int i = 0; i < 4; i++) f.add(at<void*>(self, 0xbec + 4 * i), 60, wings[i]);
    }
    uint8_t* deity = (uint8_t*)*g_deity;
    if (!deity || *(uint32_t*)deity != 0x004dc588u) { f.replay_only = "the deity isn't a RaceDeity (size unknown)"; return; }
    f.add(deity, 2012, "deity");
    const int32_t idx = at<int32_t>(self, C_CAR_INDEX);
    if ((uint32_t)idx < 16u)
        if (uint8_t* line = *(uint8_t**)(deity + 0x38 + 0x74 * idx + 0x6c)) f.add(line, 112, "centre line");
    if (*P<uint8_t>(S_AIRLIFT) && (at<uint32_t>(self, PC_AIRLIFT_TIME) & 0x7fffffff) == 0) {
        f.replay_only = "the airlift: the deity's TeleportToLine";
        return;
    }
    if (((uint8_t(__cdecl*)(const void*))0x00438660)(c + C_POS)) {       // loc_is_out_of_bounds(frame.pos)
        f.replay_only = "out of bounds: Car::Update teleports through the deity";
        return;
    }
    const uint8_t damaged = at<uint8_t>(self, 0x48c);
    const float last_damage = at<float>(self, 0x488);
    if (!((uint8_t(__cdecl*)())0x0042bd60)() && damaged && !((D(last_damage) + 2.0f) >= PhysicsGetTime()))
        f.replay_only = "Car::Update's reset_damage rebuilds the models";
}
static void fp_playcar_update(Footprint& f, void* self, Edx) {
    f.object(self, "car");
    fp_playcar_car_update(f, self);
    f.add(P<void>(S_FORCE_A), 8, "driver force a, b");                   // DriverSetForce
    f.add(P<void>(S_FORCE_C), 4, "driver force c");
    f.add(P<void>(S_GEAR), 4, "driver gear");                            // DriverSetGear
    static const uint32_t sounds[4] = {PC_SND_LEFT, PC_SND_RIGHT, PC_SND_BOTH, PC_SND_CLEAR};
    for (int i = 0; i < 4; i++)
        if (void* s = at<void*>(self, sounds[i])) f.add((uint8_t*)s + SND_PLAY, 3, "spotter sound flags");
}
PORT_FN(0x0043ea00, "PlayCar::Update", PlayCar_Update, fp_playcar_update)

// PlayCar::GetMessage: the Car's message, plus the "can teleport" bit for the HUD
static void __fastcall PlayCar_GetMessage(void* self, Edx, uint8_t* msg) {
    Car_GetMessage(self, 0, msg);
    uint8_t* mine = msg + at<int32_t>(self, C_MSG_OFFSET);
    if (CanTeleport(self)) mine[0x69] |= 0x80;
}
static void fp_playcar_get_message(Footprint& f, void* self, Edx, uint8_t* msg) {
    f.add(msg + at<int32_t>(self, C_MSG_OFFSET), 0x184, "message");     // Car::GetMessage writes to +0x182
}
PORT_FN(0x0043eef0, "PlayCar::GetMessage", PlayCar_GetMessage, fp_playcar_get_message)

// PlayCar::MakeStatusString: "", "Brake", "Gas" or "Both" (the strings at 0x4eda20) from the pedal status
static const char* status_string(uint32_t status) {
    static const uint32_t strings[4] = {0x004eda20, 0x004eda24, 0x004eda2c, 0x004eda30};
    return status < 4 ? (const char*)(uintptr_t)strings[status] : 0;
}
static void __fastcall PlayCar_MakeStatusString(void* self, Edx, char* buf) {
    const char* s = status_string(at<uint32_t>(self, C_STATUS));
    if (s) memcpy(buf, s, strlen(s) + 1);
}
static void fp_playcar_make_status_string(Footprint& f, void* self, Edx, char* buf) {
    const char* s = status_string(at<uint32_t>(self, C_STATUS));
    if (s) f.add(buf, (uint32_t)strlen(s) + 1, "status string");
}
PORT_FN(0x0043ef20, "PlayCar::MakeStatusString", PlayCar_MakeStatusString, fp_playcar_make_status_string)
