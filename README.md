# viper-port

A hook-based reimplementation of the 1998 racing game **Viper Racing** (MGI / Sierra), in the style of
OpenRCT2: a DLL loads into the original game and replaces its parts one at a time with new code, so the
game stays playable at every step. It needs the player's own copy of the game; nothing from the game is
in this repository (everything derived from its files stays in the git-ignored `out/`).

## What the DLL does

`hook/` builds `dinput.dll`, a stand-in for DirectInput that the game loads from its own folder. It
forwards `DirectInputCreateA` to Windows and, on load:

- **M1, engine fixes:** lifts the hard limits (512 physics objects, 1,024 world and graphics objects,
  the model / surface / deferred-draw pools, the 128-slot video texture pool, the 118-texture draw
  buckets, the 250-entry texture table) and replaces the all-pairs collision loop with a broad-phase.
- **M2, platform layer** (switched in `viperport.ini`, `[platform]`):
  - `sdl=1` — an SDL2 window; keyboard, mouse and game controllers through SDL.
  - `renderer=gl` — DirectDraw and Direct3D emulated on OpenGL 3.3 through the game's own imports:
    native-resolution 3D with Hor+ widescreen, the game's software HUD laid over it.
  - `audio=sdl` — DirectSound emulated on SDL2 audio.

Supported builds: v1.0 `race.exe`, v1.1 `race.bin`, and the community 1.2.4–1.2.6 `race.bin`. M1's
hooks are translated per build through generated address tables (`hook/sites_*.inc`); M2's emulations
go through the imports and need no tables.

## Layout

- `hook/` — the DLL: `viperport.cpp` (M1 and install), `platform.cpp` (window and input),
  `ddraw_gl.cpp` (renderer), `dsound_sdl.cpp` (audio); `build.bat` builds it (Visual Studio Build Tools,
  SDL2 2.32 in `../sdl2`).
- `tools/` — the analysis: linker-map extraction, the function inventory, cross-build matching
  (`match_builds.py`, `propagate.py`, `port_sites.py`), type recovery for Ghidra (`recover_types.py`,
  `type_names.py`, `names/`, `ApplyTypes.java`), and generators (`gen_com_base.py`, `gen_texture_table.py`).
