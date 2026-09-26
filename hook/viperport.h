// viperport -- what the hook's source files share.
#pragma once
#include <stddef.h>
#include <stdint.h>

// viperport.log, one timestamped line per call
void logf(const char* fmt, ...);

// is the running build v1.0 race.exe (the reference build, whose addresses need no table)?
bool build_is_v10();

// does the running build's table have this v1.0 address? (quietly; A() logs a miss)
bool have(uint32_t v10);

// the running build's address for a v1.0 race.exe address (0, logged, if its table lacks it)
uint32_t A(uint32_t v10);

// does the function at v1.0 address v10 start as it should in the running build?
bool code_is(uint32_t v10, const uint8_t* expect, size_t n);

// replace the function at v1.0 address v10 with `to`, after checking its first bytes
bool jmp_hook(uint32_t v10, const uint8_t* expect, size_t n, void* to, const char* what);

// M2 (platform.cpp): move the window and input onto SDL2 if viperport.ini says so
void platform_install(const char* build);
void platform_report();                       // the exit log's input line

// M2 stage 2 (ddraw_gl.cpp): the game's DirectDraw/Direct3D emulated on OpenGL, via its DDRAW imports
bool renderer_install();
void renderer_report();

// point race.exe's import of dll!name at `to` (M2's emulations; build-agnostic)
bool patch_import(const char* dll, const char* name, void* to);

// M2 stage 3 (dsound_sdl.cpp): the game's DirectSound emulated on SDL2 audio, via its DSOUND import
bool audio_install();
