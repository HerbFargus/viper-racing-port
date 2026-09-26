# viper-racing-port

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

## M3: rewriting the game's code, checked bit for bit

M3 replaces the game's own functions with C++, one at a time, against the v1.0 `race.exe`. The physics
runs at x87 single precision (it sets that every tick), so a faithful rewrite can match the original bit
for bit, and that is the bar. Two checks enforce it (`hook/port.h`, `port.cpp`, `replay.cpp`):

- **Per function, `[port]`:** each rewrite is registered with `PORT_FN` and switched by its map name to
  `new`, `original` or `shadow`. In shadow mode, a check runs the original, then restores the starting
  state and runs the rewrite, feeding it the same random numbers and control readings, and compares
  everything either could change byte for byte: the function's footprint, the physics and AI statics,
  the heap tables the physics owns, the return value, and its outputs (crash sounds and replay events,
  which are compared, then made once). The game always continues on the original's result. A mismatch is
  logged with the first differing field, named from the recovered types. `shadow_every=N` samples.
- **Per race, `[replay]`:** `record=1` writes each race's inputs to `replays\`: the physics ticks per
  update, the pause flag, the random numbers the physics thread draws, the 13 control readings, and the
  start-up seed the AI drivers' personalities are rolled from. `play=<name>` runs the race again with
  nobody driving. Both write a per-tick hash of every physics object; a replay compares itself to the
  recording live and logs the first tick where they part, and `tools/trace_diff.py` compares any two
  runs, field by field at the ticks `dump_ticks` names (tick 0 is always dumped).

A third check runs outside the game: `test/fuzz.exe` (`test/build_fuzz.bat`) loads the v1.0 `race.exe`'s
code at its own address, puts the FPU in the physics thread's single precision, and runs every rewrite
whose footprint is pure against its original on millions of random inputs, NaN, infinity, denormals and
overlapping arguments included.

The rewrites so far (`hook/phys_*.cpp`, layouts in `hook/phys_types.h`): the physics library's maths
helpers, every collision volume (sphere, cube, sphere group, moveable sphere, cylinder, tube, box), the
pairwise collision tests, and `collide.obj`'s collision response, water and static objects -- 73
functions, each checked in game in shadow mode or, where the game never reaches it, offline. The rules
that make bit-for-bit possible are in `hook/x87.h`: the same grouping of every sum; a value the original
keeps in an x87 register is a double, one it stores is a float; each comparison keeps the original's NaN
behaviour; each constant keeps its width.

`tools/gen_port_tables.py` generates what the DLL needs: the original instructions at every hooked
address (for trampolines), the physics classes and their named fields, and the statics of the physics
and AI object files, attributed by which file's code uses each address, with the input code and the
main thread's buffers left out.

## Layout

- `hook/` — the DLL: `viperport.cpp` (M1 and install), `port.cpp` (M3's rewrites and shadow checks),
  `phys_*.cpp` (the rewritten physics),
  `replay.cpp` (the race recorder), `platform.cpp` (window and input), `ddraw_gl.cpp` (renderer),
  `dsound_sdl.cpp` (audio); `build.bat` builds it (Visual Studio Build Tools,
  SDL2 2.32 in `../sdl2`).
- `tools/` — the analysis: linker-map extraction, the function inventory, cross-build matching
  (`match_builds.py`, `propagate.py`, `port_sites.py`), type recovery for Ghidra (`recover_types.py`,
  `type_names.py`, `names/`, `ApplyTypes.java`), generators (`gen_com_base.py`, `gen_texture_table.py`, `gen_port_tables.py`), `trace_diff.py`, and `disasm.py` (disassembly with every constant's value and width).
- `test/` — the offline fuzzer for pure rewrites (`fuzz.cpp`, `fuzz.h`, `build_fuzz.bat`) and a per-file compile check.
