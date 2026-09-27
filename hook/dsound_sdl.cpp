// viperport -- milestone M2, stage 3: the game's DirectSound, emulated on SDL2 audio.
//
// Only the game's software mixer ever runs (SoftMixer + wave.obj; the hardware DSoundMixer is never
// constructed -- out/agents/rsnd): it mixes every sound itself, 3D attenuation, panning and Doppler
// included, and streams the result into ONE looping 22050 Hz stereo buffer from its 16 ms background
// thread. So DirectSound here is a ring buffer the game writes ahead of an SDL audio callback that plays it:
//
//   CreateSoundBuffer   the primary (DSSCL_WRITEPRIMARY + SetFormat) or the 16 KB fallback secondary
//   Play / Stop         open / pause the SDL device at the buffer's format (SDL converts to the card's)
//   GetCurrentPosition  play = what the callback has consumed; write = play + ~10 ms, frame-aligned --
//                       the game's WaveGrab needs write strictly ahead of play and under its 40 ms target
//   Lock / Unlock       plain pointers into the ring (two when it wraps); an offset of exactly the size
//                       wraps to 0 instead of failing (a failed Lock cuts every one-shot sound)
//
// race.exe's DSOUND.dll!DirectSoundCreate import is pointed here (build-agnostic, like the renderer).
// Returning an error from DirectSoundCreate would just make the game silent (its NullMixer).
//
// Shadow checks (port.h): the sound code runs on the physics thread, and a check sees DirectSound the way the
// graphics' saw DirectDraw. A call that only hands back data (GetCaps, GetFormat, GetStatus, GetCurrentPosition,
// Lock) is an INPUT: made in the original's pass, its results handed to the rewrite's. A call with an effect (Unlock,
// Play, Stop, SetFormat, SetCurrentPosition, SetVolume / SetPan / SetFrequency, Restore) is an OUTPUT: recorded in
// both passes and compared, made in the original's only, its result handed on like an input. Unlock records a hash
// of the bytes written since the Lock, so the mixed audio itself is compared. Creating and releasing objects isn't
// checked (the functions that do are replay_only).
#define _CRT_SECURE_NO_WARNINGS
#include "ds5.h"
#include "com_dsound.h"
#include <atomic>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "SDL.h"
#include "viperport.h"
#include "port.h"

void com_unsupported(const char* method);

namespace {

enum { IN_DSOUND = 5 };                                          // shadow input kind (replay.cpp uses 1-3, 2+16k)
enum : uint16_t { DSREC_BUFFER = 0x7100, DSREC_DEVICE = 0x7140 }; // + the vtable slot

// an input: the rewrite's pass is handed what the original's read; the original's pass logs it
template <typename T> bool fed(T& v) { return shadow_feed(IN_DSOUND, &v, sizeof v); }
template <typename T> void saw(const T& v) { shadow_saw(IN_DSOUND, &v, sizeof v); }
// an output: recorded in both passes; true in the rewrite's, where it isn't made
bool effect(uint16_t method, const void* self, const void* args, size_t n, const void* data = 0, size_t nd = 0) {
    shadow_com_effect(method, self, args, n, data, nd);
    return shadow_com_phase() == 2;
}
// an output's result, as the original's pass got it
HRESULT result(bool skipped, HRESULT hr) {
    if (skipped) fed(hr);
    else saw(hr);
    return hr;
}

const DWORD RING = 16384;                                        // bytes, the size the game's fallback asks for

struct Buffer : Base_IDirectSoundBuffer {
    LONG refs = 1;
    bool primary = false;
    WAVEFORMATEX wfx = {};
    std::vector<uint8_t> ring;
    std::atomic<uint32_t> play{0};                               // bytes the callback has consumed, mod size
    std::atomic<bool> playing{false};
    SDL_AudioDeviceID dev = 0;
    uint32_t lead = 0;                                           // write cursor distance ahead of play
    bool global = false;                                         // DSBCAPS_GLOBALFOCUS: heard while switched away

    Buffer(bool is_primary, const WAVEFORMATEX* f) : primary(is_primary) {
        if (f) wfx = *f;
        else wfx = {WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0};
        ring.assign(RING, silence());
    }
    ~Buffer() { close(); }

    uint8_t silence() const { return wfx.wBitsPerSample == 8 ? 0x80 : 0; }
    uint32_t align() const { return wfx.nBlockAlign ? wfx.nBlockAlign : 4; }

    static void SDLCALL callback(void* self, Uint8* out, int len) {
        Buffer* b = (Buffer*)self;
        uint32_t size = (uint32_t)b->ring.size(), p = b->play.load();
        uint8_t fill = b->silence();
        for (int done = 0; done < len;) {
            uint32_t n = size - p < (uint32_t)(len - done) ? size - p : (uint32_t)(len - done);
            memcpy(out + done, &b->ring[p], n);
            memset(&b->ring[p], fill, n);                        // a stall plays silence, not the old loop
            done += n;
            p = (p + n) % size;
        }
        b->play.store(p);
        // DirectSound silenced an app's buffers while another app had the focus, unless a secondary asked for
        // DSBCAPS_GLOBALFOCUS; they play on, cursors moving, unheard
        if (!b->global && platform_switched_away()) memset(out, fill, len);
    }

    bool open() {
        if (dev) return true;
        SDL_AudioSpec want = {}, got = {};
        want.freq = (int)wfx.nSamplesPerSec;
        want.format = wfx.wBitsPerSample == 8 ? AUDIO_U8 : AUDIO_S16LSB;
        want.channels = (Uint8)wfx.nChannels;
        want.samples = 256;                                      // ~11.6 ms at 22050: the play cursor moves every tick
        want.callback = callback;
        want.userdata = this;
        dev = SDL_OpenAudioDevice(0, 0, &want, &got, 0);         // SDL converts to whatever the card wants
        if (!dev) {
            logf("audio: can't open an SDL audio device: %s", SDL_GetError());
            return false;
        }
        // write cursor ~10 ms (or one callback) ahead of play, whole frames
        uint32_t bytes_10ms = wfx.nAvgBytesPerSec / 100, cb = (uint32_t)got.samples * align();
        lead = (bytes_10ms > cb ? bytes_10ms : cb) / align() * align();
        logf("audio: %lu Hz, %d-bit, %d channel%s through SDL (%s), %u-frame callbacks",
             wfx.nSamplesPerSec, wfx.wBitsPerSample, wfx.nChannels, wfx.nChannels == 1 ? "" : "s",
             SDL_GetCurrentAudioDriver(), got.samples);
        return true;
    }
    void close() {
        if (dev) SDL_CloseAudioDevice(dev), dev = 0;
        playing = false;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDSBCAPS c) override {
        DWORD size = c->dwSize ? c->dwSize : sizeof *c;
        if (size > sizeof(DSBCAPS)) size = sizeof(DSBCAPS);
        if (fed(*c)) return DS_OK;
        memset(c, 0, size);
        c->dwSize = size;
        c->dwFlags = primary ? DSBCAPS_PRIMARYBUFFER : DSBCAPS_LOCSOFTWARE;
        c->dwBufferBytes = (DWORD)ring.size();
        saw(*c);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFormat(LPCWAVEFORMATEX f) override {
        WAVEFORMATEX a = {};
        if (f) a = *f;
        bool skip = effect(DSREC_BUFFER + 14, this, &a, sizeof a);
        return result(skip, skip ? DS_OK : set_format(f));
    }
    HRESULT set_format(LPCWAVEFORMATEX f) {
        if (!f || f->wFormatTag != WAVE_FORMAT_PCM || (f->wBitsPerSample != 8 && f->wBitsPerSample != 16)) return DSERR_BADFORMAT;
        bool was = playing;
        close();
        wfx = *f;
        ring.assign(RING / align() * align(), silence());
        play = 0;
        if (was) play_now();
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormat(LPWAVEFORMATEX f, DWORD size, LPDWORD written) override {
        struct { WAVEFORMATEX f; DWORD written; } r = {wfx, sizeof(WAVEFORMATEX)};
        if (!fed(r)) saw(r);
        if (f && size >= sizeof(WAVEFORMATEX)) *f = r.f;
        if (written) *written = r.written;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Play(DWORD r1, DWORD prio, DWORD flags) override {
        DWORD a[3] = {r1, prio, flags};
        bool skip = effect(DSREC_BUFFER + 12, this, a, sizeof a);
        return result(skip, skip ? DS_OK : play_now());
    }
    HRESULT play_now() {
        if (!open()) return DSERR_GENERIC;
        playing = true;
        SDL_PauseAudioDevice(dev, 0);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        bool skip = effect(DSREC_BUFFER + 18, this, 0, 0);
        if (!skip) {
            if (dev) SDL_PauseAudioDevice(dev, 1);
            playing = false;
        }
        return result(skip, DS_OK);
    }
    HRESULT STDMETHODCALLTYPE GetStatus(LPDWORD s) override {
        DWORD v = playing ? DSBSTATUS_PLAYING | DSBSTATUS_LOOPING : 0;
        if (!fed(v)) saw(v);
        *s = v;
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE Restore() override { return result(effect(DSREC_BUFFER + 20, this, 0, 0), DS_OK); }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(LPDWORD p, LPDWORD w) override {
        DWORD r[2];
        if (!fed(r)) {
            uint32_t size = (uint32_t)ring.size(), at = play.load();
            r[0] = at;
            r[1] = (at + (lead ? lead : align())) % size;
            saw(r);
        }
        if (p) *p = r[0];
        if (w) *w = r[1];
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD at) override {
        bool skip = effect(DSREC_BUFFER + 13, this, &at, 4);
        if (!skip) play = (at % ring.size()) / align() * align();
        return result(skip, DS_OK);
    }

    HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) override {
        struct { LPVOID p1; DWORD n1; LPVOID p2; DWORD n2; } r;
        if (!fed(r)) {
            lock(offset, bytes, &r.p1, &r.n1, &r.p2, &r.n2, flags);
            saw(r);
        }
        *p1 = r.p1;
        *n1 = r.n1;
        if (p2) *p2 = r.p2;
        if (n2) *n2 = r.n2;
        return DS_OK;
    }
    void lock(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) {
        uint32_t size = (uint32_t)ring.size();
        if (flags & DSBLOCK_FROMWRITECURSOR) offset = (play.load() + (lead ? lead : align())) % size;
        if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
        if (bytes > size) bytes = size;
        offset %= size;                                          // the game can ask for offset == size
        uint32_t first = size - offset < bytes ? size - offset : bytes;
        *p1 = &ring[offset];
        *n1 = first;
        *p2 = first < bytes ? &ring[0] : 0;
        *n2 = first < bytes ? bytes - first : 0;
    }
    // what's compared is the bytes written since the Lock (hashed; the pointers are the same in both passes)
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID p1, DWORD n1, LPVOID p2, DWORD n2) override {
        if (!shadow_com_phase()) return DS_OK;
        DWORD a[4] = {(DWORD)(uintptr_t)p1, n1, (DWORD)(uintptr_t)p2, n2};
        uint64_t h = 1469598103934665603ull;
        for (DWORD i = 0; p1 && i < n1; i++) h = (h ^ ((const uint8_t*)p1)[i]) * 1099511628211ull;
        for (DWORD i = 0; p2 && i < n2; i++) h = (h ^ ((const uint8_t*)p2)[i]) * 1099511628211ull;
        return result(effect(DSREC_BUFFER + 19, this, a, sizeof a, &h, sizeof h), DS_OK);
    }

    // the game never sets these on its one stream (its mixer does volume, pan and pitch itself)
    HRESULT STDMETHODCALLTYPE SetVolume(LONG v) override { return result(effect(DSREC_BUFFER + 15, this, &v, 4), DS_OK); }
    HRESULT STDMETHODCALLTYPE SetPan(LONG v) override { return result(effect(DSREC_BUFFER + 16, this, &v, 4), DS_OK); }
    HRESULT STDMETHODCALLTYPE SetFrequency(DWORD v) override { return result(effect(DSREC_BUFFER + 17, this, &v, 4), DS_OK); }
};

struct DirectSound : Base_IDirectSound {
    LONG refs = 1;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DS_OK; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDSCAPS c) override {
        DWORD size = c->dwSize ? c->dwSize : sizeof *c;
        if (size > sizeof(DSCAPS)) size = sizeof(DSCAPS);
        if (fed(*c)) return DS_OK;
        memset(c, 0, size);
        c->dwSize = size;
        c->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                     DSCAPS_CONTINUOUSRATE | DSCAPS_EMULDRIVER;
        c->dwMinSecondarySampleRate = 100, c->dwMaxSecondarySampleRate = 100000;
        c->dwPrimaryBuffers = 1;
        saw(*c);
        return DS_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSoundBuffer(LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN) override {
        // the game passes the DirectX 5 DSBUFFERDESC (0x14 bytes, no 3D algorithm GUID)
        bool primary = (d->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
        if (!primary && d->lpwfxFormat && d->lpwfxFormat->wFormatTag != WAVE_FORMAT_PCM) return DSERR_BADFORMAT;
        Buffer* b = new Buffer(primary, primary ? 0 : d->lpwfxFormat);
        b->global = !primary && (d->dwFlags & DSBCAPS_GLOBALFOCUS);
        *out = b;
        return DS_OK;
    }
};

HRESULT WINAPI emu_DirectSoundCreate(LPGUID, LPDIRECTSOUND* out, LPUNKNOWN) {
    static bool started;
    if (!started) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            logf("audio: SDL audio won't start (%s): the game runs silent", SDL_GetError());
            *out = 0;
            return DSERR_NODRIVER;
        }
        started = true;
    }
    *out = new DirectSound;
    return DS_OK;
}

}  // namespace

bool audio_install() {
    if (!patch_import("DSOUND.dll", "DirectSoundCreate", (void*)emu_DirectSoundCreate)) {
        logf("audio: NOT switching to SDL -- the DSOUND import wasn't found");
        return false;
    }
    // the game's sound thread sleeps 16 ms a tick and never asks for a fine timer; at Windows' default
    // 15.6 ms granularity a tick can stretch to ~31 ms, close to its 40 ms of queued audio
    timeBeginPeriod(1);
    logf("audio: SDL2 (DirectSound emulated)");
    return true;
}
