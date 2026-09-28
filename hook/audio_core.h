// audio_core.h -- M3 sound, step S2: the game's DirectSound as plain functions, on SDL2 audio (dsound_sdl.cpp).
//
// Two ways in, one audio core:
//   * wave.obj's rewrites (hook/snd_mix.cpp: WaveBegin, create_primary_buffer, create_secondary_buffer, WaveEnd,
//     WaveGrab, WaveRelease) call the functions below directly;
//   * the COM facade (dsound_sdl.cpp: race.exe's DSOUND!DirectSoundCreate import is pointed at it) serves the same
//     objects as IDirectSound / IDirectSoundBuffer to the original code: the race.bin builds, `original` mode, any
//     wave.obj function left original, and a shadow check's original pass. Each COM method the game uses is one call
//     into the function of the same name here.
// A handle IS the facade's object (a Device is the IDirectSound*, a Buffer the IDirectSoundBuffer*), so the wave.obj
// statics (the DirectSound at 0x578c90, the buffer at 0x578c9c) hold the same objects whichever code made them, and
// original and rewritten wave.obj functions mix freely.
//
// Each function does exactly what its COM method does, with the same arguments and results (DS_OK = 0, DSERR_*),
// including the shadow recording (port.h): the inputs (device_caps, buffer_caps, status, position, lock) are fed /
// saw, the effects (set_format, play, stop, restore, unlock) recorded with the same method id (the IDirectSoundBuffer
// vtable slot), object, argument bytes and Unlock hash -- so a call made through COM and the same call made here leave
// byte-identical records, and a check's two passes can take either way. Creating and releasing aren't recorded.
//
// Only this file's declarations are needed to call the core: no windows.h, no dsound.h (the types are the SDK's, by
// their tags; long = HRESULT, unsigned long = DWORD / ULONG).
#pragma once

struct IDirectSound;
struct IDirectSoundBuffer;
struct _DSCAPS;
struct _DSBCAPS;
struct _DSBUFFERDESC;
struct tWAVEFORMATEX;

namespace audio {

typedef IDirectSound* Device;              // the facade's IDirectSound
typedef IDirectSoundBuffer* Buffer;        // the facade's IDirectSoundBuffer

// ---- the device --------------------------------------------------------------------------------------------------
// DirectSoundCreate(NULL, out, NULL): starts SDL audio the first time (failing: *out = 0, DSERR_NODRIVER -- the game
// then runs silent) and makes a device
long create(Device* out);
long set_cooperative_level(Device ds, void* hwnd, unsigned long level);           // DS_OK: SDL has no such thing
long device_caps(Device ds, _DSCAPS* caps);                                        // an input
// CreateSoundBuffer(desc, out, NULL): the primary (DSBCAPS_PRIMARYBUFFER; 22050 Hz 16-bit stereo until set_format) or
// a secondary in desc's PCM format (another format: DSERR_BADFORMAT), a 16 KB ring either way
long create_buffer(Device ds, const _DSBUFFERDESC* desc, Buffer* out);
unsigned long release(Device ds);

// ---- a buffer ----------------------------------------------------------------------------------------------------
long set_format(Buffer b, const tWAVEFORMATEX* f);                                 // an effect (8/16-bit PCM only)
long buffer_caps(Buffer b, _DSBCAPS* caps);                                        // an input
long play(Buffer b, unsigned long reserved, unsigned long priority, unsigned long flags);   // an effect: always loops
long stop(Buffer b);                                                               // an effect
long status(Buffer b, unsigned long* s);                                           // an input
long restore(Buffer b);                                                            // an effect (nothing is ever lost)
long position(Buffer b, unsigned long* play, unsigned long* write);                // an input: the two cursors
// an input: pointers into the ring (two when it wraps); an offset of exactly the size wraps to 0
long lock(Buffer b, unsigned long offset, unsigned long bytes, void** p1, unsigned long* n1, void** p2,
          unsigned long* n2, unsigned long flags);
long unlock(Buffer b, void* p1, unsigned long n1, void* p2, unsigned long n2);     // an effect: hashes what was written
unsigned long release(Buffer b);

}  // namespace audio
