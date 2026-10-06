// phys_collide.cpp -- M3 3.2: collide.obj (what a collision sets off: the crash events and sounds, the
// water splashes) and volume.obj's static-object list, rewritten.
//
// Collide() applies no impulse itself (the volumes' own Collide methods do that before calling it): it
// posts the crash to the main thread's event queue (PhysTaskEvent: the sparks and the damage) and plays a
// crash sound through a ring of 8 CollisionSounds on the heap, skipping a pair of volumes that already
// crashed within half a second. CollideWater() does the same for a splash, with a 32-entry ring of recent
// splashes in collide.obj's statics and one shared splash sound.
//
// CollisionSound::Play (both) and PhysReplayAddEvent are the shadow framework's outputs (port.cpp,
// install_outputs): they're called here by address, exactly as the originals call them, and the check
// records and compares them instead of running them twice.
//
// Divisions: none. Constants are the originals' floats (all dword operands here).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "phys_types.h"
#include "x87.h"

#define D(x) ((double)(x))                          // a register value (x87.h)

// ---- the engine's own types here -----------------------------------------------------------------------
// CollisionSound (0x28 bytes): the crash-sound ring entry. Collide() fills time and the pair; Play(pos,
// vel) keeps the position at +0x1c and hands a loudness to the Sound3D, which was created over velocity
// (+0x10, zeroed, never set) and position.
struct CollisionSound {
    float time;                        // +0    when this pair last played
    CollisionVolume* a;                // +4
    CollisionVolume* b;                // +8
    void* sound;                       // +0xc  Sound3D*
    P3 velocity;                       // +0x10
    P3 position;                       // +0x1c
};
static_assert(offsetof(CollisionSound, sound) == 0xc && sizeof(CollisionSound) == 0x28, "CollisionSound");

// the splash ring (collide.obj statics, 0x521e78)
struct Splash { CollisionVolume* volume; float time; };

// a static object of the track's .sol file (0xe0 bytes): its frame, an id, the volume's FourCC and the
// volume itself, built in place in the storage (the largest, a TubeVolume, is 0xa4 bytes)
struct StaticObject {
    Frame frame;                       // +0
    int32_t id;                        // +0x30  -1 when there is no volume
    uint32_t type_tag;                 // +0x34  'BOX ', 'SPHR', 'TUBE'; 0 = none
    CollisionVolume* volume;           // +0x38  -> storage once loaded
    uint8_t storage[0xa4];             // +0x3c
};
static_assert(offsetof(StaticObject, type_tag) == 0x34 && sizeof(StaticObject) == 0xe0, "StaticObject");

// the .sol resource as loaded: this header, then count objects, then count2 16-bit words; the pointers
// are fixed up the first time the resource is loaded
struct StaticObjectList {
    int32_t count;                     // +0
    int32_t count2;                    // +4
    StaticObject* objects;             // +8   -> +0x14
    uint16_t* words;                   // +0xc -> after the objects
    uint8_t* end;                      // +0x10 -> after the words
};
static_assert(sizeof(StaticObjectList) == 0x14, "StaticObjectList");

// ---- statics ---------------------------------------------------------------------------------------------
#define g_sound         (*(CollisionSound**)0x00521e30)     // the crash-sound ring (8, heap)
#define g_sound_index   (*(int32_t*)0x00521e34)             // its next slot
#define g_splash        ((Splash*)0x00521e78)               // [32]
#define g_splash_end    ((Splash*)0x00521f78)
#define g_splash_index  (*(int32_t*)0x00521f78)
#define g_splash_sound  (*(uint8_t**)0x00521f7c)            // Sound3D*
#define g_splash_pos    ((P3*)0x00521f80)
#define g_splash_vel    ((P3*)0x00521f8c)
#define g_next_sound    (*(int32_t*)0x004ed894)             // CollisionSound::next_sound

// ---- the engine's functions, by address ---------------------------------------------------------------
typedef void(__fastcall* GetPointVelocity_t)(void* self, void* edx, P3* out, const P3* point);
typedef void(__cdecl* PhysTaskEvent_t)(unsigned char type, const void* data, int size);
typedef void(__cdecl* PhysReplayAddEvent_t)(int type, const void* data, int size);
typedef float(__cdecl* PhysicsGetTime_t)();
typedef void(__fastcall* PlayVel_t)(void* self, void* edx, const P3* pos, const P3* vel);
typedef void(__fastcall* PlayF_t)(void* self, void* edx, const P3* pos, uint32_t loud_bits);   // a float, by its bits
typedef void*(__cdecl* MemAlloc_t)(int bytes);
typedef void(__cdecl* OpDelete_t)(void* p);
typedef void*(__cdecl* Sound3DCreate_t)(const char* name, int flags, const P3* vel, const P3* pos);
typedef void*(__fastcall* CollisionSoundCtor_t)(void* self, void* edx);
typedef void(__fastcall* CollisionSoundDtor_t)(void* self, void* edx);
typedef int(__cdecl* ReplayHandler_t)(const void* data, int mode);
typedef void(__cdecl* InstallHandler_t)(int event, ReplayHandler_t fn);
typedef void(__cdecl* UninstallHandler_t)(ReplayHandler_t fn);
typedef void*(__cdecl* ResourceTry_t)(const char* name, uint32_t type, uint32_t* size, int* a, uint8_t* fresh, uint8_t* b);
typedef uint8_t(__cdecl* ResourceForget_t)(void* p);
typedef void(__cdecl* LogPanic_t)(const char* fmt, ...);
typedef void*(__fastcall* VolumeCtor_t)(void* self, void* edx, const Frame* frame);

static const GetPointVelocity_t GetPointVelocity = (GetPointVelocity_t)0x00444ee0;
static const PhysTaskEvent_t PhysTaskEvent = (PhysTaskEvent_t)0x00426e40;
static const PhysReplayAddEvent_t PhysReplayAddEvent = (PhysReplayAddEvent_t)0x0042d8b0;   // (output)
static const PhysicsGetTime_t PhysicsGetTime = (PhysicsGetTime_t)0x0042bc80;
static const PlayVel_t CollisionSoundPlayVel = (PlayVel_t)0x0043c560;                        // (output)
static const PlayF_t CollisionSoundPlayF = (PlayF_t)0x0043c510;                              // (output)
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
static const OpDelete_t OpDelete = (OpDelete_t)0x00414390;
static const Sound3DCreate_t Sound3DCreate = (Sound3DCreate_t)0x00472560;
static const CollisionSoundCtor_t CollisionSoundCtor = (CollisionSoundCtor_t)0x0043c480;
static const CollisionSoundDtor_t CollisionSoundDtor = (CollisionSoundDtor_t)0x0043c4f0;
static const InstallHandler_t PhysReplayInstallEventHandler = (InstallHandler_t)0x0042d960;
static const UninstallHandler_t PhysReplayUninstallEventHandler = (UninstallHandler_t)0x0042d990;
static const ResourceTry_t ResourceTry = (ResourceTry_t)0x00419ce0;
static const ResourceForget_t ResourceForget = (ResourceForget_t)0x0041a450;
static const LogPanic_t LogPanic = (LogPanic_t)0x004112b0;
static const VolumeCtor_t BoxVolumeCtor = (VolumeCtor_t)0x00433a10;
static const VolumeCtor_t SphereVolumeCtor = (VolumeCtor_t)0x004323a0;
static const VolumeCtor_t TubeVolumeCtor = (VolumeCtor_t)0x004337a0;

// the handlers are installed by their ORIGINAL addresses, as the original does (a rewrite hooked there is
// what runs, and the replay's handler table holds the same values either way)
static const ReplayHandler_t k_crash_handler = (ReplayHandler_t)0x0043c6c0;
static const ReplayHandler_t k_splash_handler = (ReplayHandler_t)0x0043c6a0;

// ---- Collide (collide.obj 0x43c710) ------------------------------------------------------------------
// a and b have met at `point`, with relative velocity `rel_vel` along `normal`. The velocity of a's owner
// at the point is v. A hard enough hit (|rel_vel|^2 > 10000 and |v|^2 > 49) posts event 2: v with its
// normal part removed, the point and rel_vel (the sparks); |v|^2 > 25 posts event 1: v and the point.
// Then the crash sound, unless this same pair (in this order) played within the last half second.
static void __cdecl Collide_rw(CollisionVolume* a, CollisionVolume* b, const P3* point, const P3* rel_vel,
                               const P3* normal) {
    P3 v;
    GetPointVelocity(a->owner, 0, &v, point);
    // fcomp; test ah,0x41; jne skip: both go on only when ordered and greater
    if ((D(rel_vel->x) * rel_vel->x + D(rel_vel->y) * rel_vel->y) + D(rel_vel->z) * rel_vel->z > 10000.0f &&
        (D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x > 49.0f) {
        // the dot product and its negation stay in a register; the three results are stored
        double nd = -((D(normal->x) * v.x + D(normal->y) * v.y) + D(normal->z) * v.z);
        float ev[9];
        ev[0] = (float)(D(normal->x) * nd + v.x);
        ev[1] = (float)(D(normal->y) * nd + v.y);
        ev[2] = (float)(nd * normal->z + v.z);
        memcpy(&ev[3], point, 12);                      // integer copies
        memcpy(&ev[6], rel_vel, 12);
        PhysTaskEvent(2, ev, 0x24);
    }
    if ((D(v.y) * v.y + D(v.z) * v.z) + D(v.x) * v.x > 25.0f) {
        float ev[6];
        memcpy(&ev[0], &v, 12);
        memcpy(&ev[3], point, 12);
        PhysTaskEvent(1, ev, 0x18);
    }
    float t = PhysicsGetTime();                         // fstp dword
    CollisionSound* s = g_sound;                        // read once for the search
    for (int i = 0; i < 8; i++) {
        // fld t; fsub 0.5f; fcomp time; test ah,1 (C0): (t - 0.5) < time, or unordered -> still sounding
        if (s[i].a == a && s[i].b == b && !(D(t) - 0.5f >= s[i].time)) return;
    }
    memcpy(&g_sound[g_sound_index].time, &t, 4);
    g_sound[g_sound_index].a = a;
    g_sound[g_sound_index].b = b;
    CollisionSoundPlayVel(&g_sound[g_sound_index], 0, point, rel_vel);
    g_sound_index = g_sound_index + 1;
    g_sound_index = g_sound_index % 8;                  // cdq/xor/sub/and 7/xor/sub: C's signed remainder
}
// writes only statics (the event queue, the ring index), the crash-sound heap block and outputs
static void fp_collide(Footprint& f, CollisionVolume*, CollisionVolume*, const P3*, const P3*, const P3*) {}
PORT_FN(0x0043c710, "Collide", Collide_rw, fp_collide)

// ---- CollideWater (collide.obj 0x43c950) ---------------------------------------------------------------
// volume hit the water at `pos` with velocity `vel`. A volume that splashed within the last second is
// skipped; so is one not moving down (the sign bit of vel.y set and not -0: an integer compare of its
// bits), or slower than |vel|^2 = 4.44. Otherwise event 0 (volume, pos) goes to the main thread and the
// replay, the ring records it, and the splash sound is set to that place at a loudness of 0.05625 |vel|,
// at most 1.
static void __cdecl CollideWater_rw(CollisionVolume* volume, P3* pos, const P3* vel) {
    float t = PhysicsGetTime();                         // fstp dword
    for (Splash* e = g_splash; e < g_splash_end; e++) {
        // fld t; fsub time; fcomp 1.0f; test ah,1 (C0): less than a second ago, or unordered
        if (e->volume == volume && !(D(t) - e->time >= 1.0f)) return;
    }
    uint32_t vy;
    memcpy(&vy, &vel->y, 4);
    if (vy <= 0x80000000u) return;                      // cmp [esi+4],0x80000000; jbe
    if (!((D(vel->x) * vel->x + D(vel->y) * vel->y) + D(vel->z) * vel->z > (float)4.44444466f)) return;   // test ah,0x41
    uint32_t ev[7];
    memcpy(&ev[0], vel, 12);                            // integer copies
    memcpy(&ev[3], pos, 12);
    memcpy(&ev[6], &volume, 4);
    PhysTaskEvent(0, ev, 0x1c);
    PhysReplayAddEvent(3, ev, 0x1c);
    memcpy(&g_splash[g_splash_index].time, &t, 4);
    g_splash[g_splash_index].volume = volume;
    g_splash_index = g_splash_index + 1;
    g_splash_index = g_splash_index & 0x1f;
    if (g_splash_sound == 0) return;
    memcpy(g_splash_vel, vel, 12);
    memcpy(g_splash_pos, pos, 12);
    double s = x87_sqrt((D(vel->x) * vel->x + D(vel->y) * vel->y) + D(vel->z) * vel->z) * (float)0.0562499985f;
    // fld 1.0f; fcom st(1); test ah,0x41; je: s is kept only when 1 > s (ordered), else 1
    double loud = (1.0f > s) ? s : 1.0f;
    float* volume_field = (float*)(g_splash_sound + 8);
    float stored = (float)loud;                         // fstp dword, after the compare
    // fcom [volume]; test ah,0x40 (C3): equal, or unordered -> unchanged
    if (loud < *volume_field || loud > *volume_field) {
        g_splash_sound[0x2c] = 1;
        *volume_field = stored;
    }
    g_splash_sound[0x29] = 1;
    g_splash_sound[0x2b] = 0;
    g_splash_sound[0x2a] = 0;
}
// besides the statics: the splash Sound3D's loudness (+8) and flags (+0x29..+0x2c)
static void fp_collide_water(Footprint& f, CollisionVolume*, P3*, const P3*) {
    if (uint8_t* snd = g_splash_sound) {
        f.add(snd + 8, 4, "splash Sound3D loudness");
        f.add(snd + 0x29, 4, "splash Sound3D flags");
    }
}
PORT_FN(0x0043c950, "CollideWater", CollideWater_rw, fp_collide_water)

// ---- CollideBegin (collide.obj 0x43c600) -------------------------------------------------------------
// new CollisionSound[8] (MemAlloc, the count in the dword before the array), the splash statics cleared,
// the splash sound created over them, and the two replay handlers installed
static void __cdecl CollideBegin_rw() {
    uint32_t* block = (uint32_t*)MemAlloc(0x144);
    if (block != 0) {
        CollisionSound* s = (CollisionSound*)(block + 1);
        block[0] = 8;
        for (int i = 0; i < 8; i++) CollisionSoundCtor(&s[i], 0);
        g_sound = s;
    } else {
        g_sound = 0;
    }
    g_sound_index = 0;
    memset(g_splash, 0, 0x48 * 4);                      // rep stosd: the ring, its index, the sound, pos, vel
    g_splash_sound = (uint8_t*)Sound3DCreate((const char*)0x004ed8d0, 4, g_splash_vel, g_splash_pos);
    PhysReplayInstallEventHandler(1, k_crash_handler);
    PhysReplayInstallEventHandler(3, k_splash_handler);
}
static void fp_collide_begin(Footprint& f) { f.replay_only = "allocates the crash-sound ring and creates sounds"; }
PORT_FN(0x0043c600, "CollideBegin", CollideBegin_rw, fp_collide_begin)

// ---- CollideEnd (collide.obj 0x43caf0) -----------------------------------------------------------------
// the reverse; the splash sound is deleted but its pointer is left as it was (as in the original)
static void __cdecl CollideEnd_rw() {
    PhysReplayUninstallEventHandler(k_crash_handler);
    PhysReplayUninstallEventHandler(k_splash_handler);
    if (g_splash_sound != 0) VFN(g_splash_sound, 0, void*, unsigned)(g_splash_sound, 0, 1);   // deleting dtor
    if (g_sound != 0) {
        uint32_t* block = (uint32_t*)g_sound - 1;
        int n = (int)block[0];
        CollisionSound* e = g_sound + n;
        for (int i = n - 1; i >= 0; i--) {              // dec; js / dec; jns: last to first
            e--;
            CollisionSoundDtor(e, 0);
        }
        OpDelete(block);
    }
    g_sound = 0;
}
static void fp_collide_end(Footprint& f) { f.replay_only = "frees the crash-sound ring and deletes sounds"; }
PORT_FN(0x0043caf0, "CollideEnd", CollideEnd_rw, fp_collide_end)

// ---- CollisionSound::CollisionSound (collide.obj 0x43c480) -------------------------------------------
// the three crash samples in turn (next_sound counts 1, 2, 0, ...); time and the P3 at +0x1c are left
static CollisionSound* __fastcall CollisionSound_ctor_rw(CollisionSound* self, void*) {
    const char* const names[3] = {(const char*)0x004ed898, (const char*)0x004ed8a4, (const char*)0x004ed8b0};
    g_next_sound = g_next_sound + 1;
    int k = g_next_sound % 3;                           // idiv: C's remainder
    g_next_sound = k;
    self->sound = Sound3DCreate(names[k], 4, &self->velocity, &self->position);
    self->velocity.x = 0.0f;                            // (integer zero stores)
    self->velocity.y = 0.0f;
    self->velocity.z = 0.0f;
    self->b = 0;
    self->a = 0;
    return self;
}
static void fp_collision_sound_ctor(Footprint& f, CollisionSound*, void*) { f.replay_only = "creates a Sound3D"; }
PORT_FN(0x0043c480, "CollisionSound::CollisionSound", CollisionSound_ctor_rw, fp_collision_sound_ctor)

// ---- CollisionSound::~CollisionSound (collide.obj 0x43c4f0) ------------------------------------------
static void __fastcall CollisionSound_dtor_rw(CollisionSound* self, void*) {
    if (self->sound != 0) {
        VFN(self->sound, 0, void*, unsigned)(self->sound, 0, 1);   // deleting dtor
        self->sound = 0;
    }
}
static void fp_collision_sound_dtor(Footprint& f, CollisionSound*, void*) { f.replay_only = "deletes a Sound3D"; }
PORT_FN(0x0043c4f0, "CollisionSound::~CollisionSound", CollisionSound_dtor_rw, fp_collision_sound_dtor)

// ---- replay_splash_handler (collide.obj 0x43c6a0) --------------------------------------------------------
// a recorded splash, played back: the same event 0 to the main thread; returns the event's size
static int __cdecl replay_splash_handler_rw(const void* data, int) {
    PhysTaskEvent(0, data, 0x1c);
    return 0x1c;
}
static void fp_replay_splash(Footprint& f, const void*, int) {}   // the event queue is a static
PORT_FN(0x0043c6a0, "replay_splash_handler", replay_splash_handler_rw, fp_replay_splash)

// ---- replay_crash_handler (collide.obj 0x43c6c0) -------------------------------------------------------------
// a recorded crash sound (Play(pos, vel)'s event 1: a tag, the position at +4, the loudness at +0x10),
// played through the next ring slot; returns the event's size
static int __cdecl replay_crash_handler_rw(const void* data, int) {
    const uint8_t* d = (const uint8_t*)data;
    uint32_t loud;
    memcpy(&loud, d + 0x10, 4);                         // pushed as a dword
    CollisionSoundPlayF(&g_sound[g_sound_index], 0, (const P3*)(d + 4), loud);
    g_sound_index = g_sound_index + 1;
    g_sound_index = g_sound_index % 8;
    return 0x14;
}
static void fp_replay_crash(Footprint& f, const void*, int) {}   // the index is a static; Play is an output
PORT_FN(0x0043c6c0, "replay_crash_handler", replay_crash_handler_rw, fp_replay_crash)

// ---- StaticObjectListGet (volume.obj 0x435f00) -------------------------------------------------------------
// the track's static objects ('SOBL' resource, track.sol). The first time the resource is loaded, its
// pointers are fixed up and each object's volume is built in place from its FourCC, updated, updated
// again and given surface 110. An object with no FourCC gets none -- and the second update then calls
// through its null volume, as in the original (the track files never have one).
static StaticObjectList* __cdecl StaticObjectListGet_rw(const char* name) {
    uint8_t fresh = 0;
    uint32_t size;
    StaticObjectList* l = (StaticObjectList*)ResourceTry(name, 0x534f424c, &size, 0, &fresh, 0);
    if (l == 0) {
        LogPanic((const char*)0x004ed75c);
        return 0;
    }
    if (!fresh) return l;
    l->objects = (StaticObject*)((uint8_t*)l + 0x14);
    l->words = (uint16_t*)((uint8_t*)l->objects + l->count * 0xe0);
    l->end = (uint8_t*)l->words + l->count2 * 2;
    for (int i = 0; i < l->count; i++) {                // count re-read each pass
        // the original tests objects + i for null (placement new): kept, as an integer so it stays
        uintptr_t op = (uintptr_t)l->objects + (uintptr_t)i * 0xe0;
        if (op != 0) {
            StaticObject* o = (StaticObject*)op;
            if (o->type_tag != 0) {
                o->volume = (CollisionVolume*)o->storage;
                // (the original also writes type_tag and id back unchanged here)
                switch (o->type_tag) {
                case 0x424f5820:                        // 'BOX '
                    if (o->volume) BoxVolumeCtor(o->volume, 0, &o->frame);
                    break;
                case 0x53504852:                        // 'SPHR'
                    if (o->volume) SphereVolumeCtor(o->volume, 0, &o->frame);
                    break;
                case 0x54554245:                        // 'TUBE'
                    if (o->volume) TubeVolumeCtor(o->volume, 0, &o->frame);
                    break;
                }
                VFN(o->volume, 4, void)(o->volume, 0);  // Update
            } else {
                o->type_tag = 0;
                o->volume = 0;
                o->id = -1;
            }
        }
        CollisionVolume* v = l->objects[i].volume;
        VFN(v, 4, void)(v, 0);                          // Update, again
        v = l->objects[i].volume;
        VFN(v, 0x1c, void, int)(v, 0, 0x6e);            // SetSurfaceType(110)
    }
    return l;
}
static void fp_static_list_get(Footprint& f, const char*) { f.replay_only = "loads track.sol through the resource cache"; }
PORT_FN(0x00435f00, "StaticObjectListGet", StaticObjectListGet_rw, fp_static_list_get)

// ---- StaticObjectListForget (volume.obj 0x436040) ----------------------------------------------------------
static void __cdecl StaticObjectListForget_rw(StaticObjectList* l) {
    ResourceForget(l);
}
static void fp_static_list_forget(Footprint& f, StaticObjectList*) { f.replay_only = "releases a resource"; }
PORT_FN(0x00436040, "StaticObjectListForget", StaticObjectListForget_rw, fp_static_list_forget)
