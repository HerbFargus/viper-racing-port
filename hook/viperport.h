// viperport -- what the hook's source files share.
#pragma once
#include <stddef.h>
#include <stdint.h>

// viperport.log, one timestamped line per call
void logf(const char* fmt, ...);

// M1 (viperport.cpp, relocate_res_tables): the options, language and open-file tables moved into the DLL, v1.0 only,
// and their capacities there (stock: 256 options, 8 languages, 32 open files -- each loaded resource set keeps one).
// The rewrites (krn_res.cpp, krn_file.cpp) stop at them.
enum { VP_LIFT_OPTIONS = 4096, VP_LIFT_LANGUAGES = 64, VP_LIFT_FILES = 256 };
// M1 (viperport.cpp, relocate_graf_lists): the track graph's two facing-model lists (stock 512 at 0x558968 and 514 at
// 0x558160, filled by draw_tree with no bound) moved into the DLL, v1.0 only; the rewrite (wld_draw.cpp) stops at it.
enum { VP_LIFT_GRAF_FACING = 8192 };
// the sound manager's table and the software mixer's bag of sounds (viperport.cpp lift_sound_limits; snd_mgr / snd_mix)
enum { VP_LIFT_SOUNDS = 1024 };

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
bool platform_plans_gl(const char* ini);     // will platform_install switch to the OpenGL renderer?
bool platform_switched_away();                // the game's inactive flag (win32.obj): DirectSound goes quiet while it's set
void gfx_repaint();                           // the OpenGL renderer shows its picture again (gl_core.cpp)
void renderer_report();

// point race.exe's import of dll!name at `to` (M2's emulations; build-agnostic)
bool patch_import(const char* dll, const char* name, void* to);

// M2 stage 3 (dsound_sdl.cpp): the game's DirectSound emulated on SDL2 audio, via its DSOUND import
bool audio_install();
bool platform_plans_sdl_audio(const char* ini);  // will platform_install switch to the SDL audio? (platform.cpp)
