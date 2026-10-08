# viper-racing-port

A reimplementation of the 1998 racing game **Viper Racing** (MGI / Sierra): every function of the
v1.0 `race.exe` rewritten in C++, checked bit for bit against the original, on a modern platform
layer -- OpenGL at your screen's own resolution with widescreen, SDL2 sound and input, the original's
hard limits lifted. It runs on Windows and natively on Linux (no Wine), and multiplayer works between
the two.

It needs your own copy of the game. **Nothing from the game is in this repository**: the engine reads
your `race.exe` and the game's data files from your install, and everything derived from them while
developing stays in the git-ignored `out/`.

Not affiliated with MGI, Sierra or their successors. Viper Racing is (c) Sierra / MGI.

## Play it

Download the engine from [Releases](https://github.com/HerbFargus/viper-racing-port/releases):

| download | for |
|---|---|
| `viperport-windows-<version>.zip` | Windows: `viperport.exe`, `dinput.dll`, `SDL2.dll`, `viperport.ini` and a readme. Copy them beside `race.exe`, then run `viperport.exe` (v1.0), or start a v1.1 / community 1.2.x game as usual (the engine loads as `dinput.dll`). |
| `viperport-linux-<version>.tar.gz` | Linux (x86, glibc 2.35 or newer): a native build with SDL2 included and `README-linux.txt` (the one-line 32-bit dependency install per distro). Unpack into the game folder and run `./viperport.sh`. |

Or let the [Viper Racing Mod Manager](https://github.com/HerbFargus/viper-racing-modding) install it
for you on Windows (its **Modern engine** panel and **Play** button).

What you need: Viper Racing v1.0 (`race.exe` from the CD) for the full engine (`viperport.exe` /
`viperport` on Linux); v1.1 and the community 1.2.4–1.2.6 `race.bin` builds get the platform layer
and lifted limits through `dinput.dll` on Windows.

## Build it

- **Windows (MSVC, the shipped build):** Visual Studio 2026 Build Tools (C++), and the SDL2 2.32.10
  Visual C++ development package unpacked beside this repository as `..\sdl2\SDL2-2.32.10`. Then
  `hook\build.bat` (the engine, `hook\build\dinput.dll`) and `loader\build_loader.bat` (the standalone,
  `loader\build\viperport.exe`).
- **Windows (GCC):** MSYS2's mingw32 toolchain (`C:\msys64\mingw32`): `hook\build_gcc.bat`,
  `loader\build_loader_gcc.bat`.
- **Linux:** an i386-multilib GCC 15 with `libsdl2-dev:i386`: `tools/build_linux.sh` (out-linux/viperport).
  The release build (glibc 2.35, SDL2 bundled) is `tools/build_linux.sh release` on Ubuntu 22.04 with
  GCC 15 from the ubuntu-toolchain-r PPA and SDL2 2.32.10 built for i386 in `/opt/sdl2-i386`;
  `tools/package_linux.sh` packs it. `tools/package_windows.py` packs the Windows zip.
- **Testing:** the world harnesses in `test/` (`python test\sweep.py` builds and runs them with both
  Windows compilers) and the session-replay corpus (`tools/corpus.py`) need your own game files; see
  `docs/PORTING.md`.

## Licence

GPL-3.0 (`LICENSE`). The packages bundle SDL2 (zlib licence) and, on Linux, link GCC's runtime under
the GCC Runtime Library Exception.

---

The rest of this README is the developer's view: how the engine hooks the game, how each rewrite is
checked, and the project's layout.

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
for bit, and that is the bar. Three checks in game enforce it (`hook/port.h`, `port.cpp`, `replay.cpp`, `session.cpp`):

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
- **Per session, `[session]`** (`hook/session.cpp`): `record=1` records a whole run of the game, menus
  included, to `sessions\<time>\`: everything the main thread reads that can differ between two runs, in
  the order it reads it (the input the platform layer hands the game, every clock, the main thread's
  random numbers and the start-up seed), and a per-frame hash of what the game hands the renderer (the 2D
  page, also as a 4 x 4 grid of tiles, the surfaces it writes, the 3D state and the 3D work). The user
  folder (`Config\`) is copied into the session when the game names it. `play=<name>` (with `label=`)
  replays it on a fresh start, on its own copy of that folder, with real input ignored, and compares
  every frame live: "IDENTICAL over all N frames" or the first frame where they part and what differed;
  `tools/session_diff.py` compares any two traces. A race inside a session runs in lockstep (each physics
  update waits for the main thread's sync points, and a replay lets the same updates run at the same
  points), so its frames are compared too, and it is handed to the race recorder. This is the check for
  code the menus run, which the race recorder doesn't reach. Network games are recorded too
  (`hook/net_wsock.cpp`): every value the game reads from winsock, on the thread that reads it (the menus,
  the multiplayer lobby task, the physics thread), and every packet it sends, which a replay compares and
  hashes into the frames. A replay makes no real network call. The lobby task ticks at the main thread's
  Win32Idle calls and a network race runs in lockstep, so their frames are compared like any other. With
  `[test] two_copies=1` (see "Testing multiplayer on one PC") copy 2 records to `sessions-2\` and binds port
  2002; copy 1 keeps 2001 and also sends its broadcasts to 2002, so copy 1 hosts and copy 2 joins (by the
  LAN list or copy 1's address). Replay each copy's recording on its own, with one copy running and
  `two_copies=1` still set (the window must be the recording's size): `play=<name>` also looks in
  `sessions-2\`, or name it outright as `play=sessions-2\<name>`.

A fourth check runs outside the game: `test/fuzz.exe` (`test/build_fuzz.bat`) loads the v1.0 `race.exe`'s
code at its own address, puts the FPU in the physics thread's single precision, and runs every rewrite
whose footprint is pure against its original on millions of random inputs, NaN, infinity, denormals and
overlapping arguments included.

The rewrites so far, 8,348 functions (the physics and AI, 881 of them, in `hook/phys_*.cpp`, layouts in
`hook/phys_types.h`):
- **3.2, collisions:** the physics library's maths helpers, every collision volume (sphere, cube, sphere
  group, moveable sphere, cylinder, tube, box), the pairwise collision tests, and `collide.obj`'s crash
  reports, water and static objects.
- **3.3, the car:** the wheel (suspension, tyre contact, brakes), the Pacejka tyre and aerodynamics, the
  drivetrain (clutch, gearbox with its automatic shifting, differentials, shafts), and the engine, its
  heat and power curve, and the hidden plane mode's propeller and wings.
- **3.4, the objects and the race:** `Car` (its tick, set-up, damage and dents, teleport, replay
  packets), the rigid body (`PhobDyno::Update`), ground contact, obstacles, wobbles, balls and
  checkpoints, the player's car and the input side (driver filtering, the control mapping), and the race
  logic (laps, positions, the start, drag races).
- **3.5, the AI:** the AI car (chasing its rabbit point along the racing line, lap-time rubber-banding,
  passing, head-on panic, the stranded-car teleport, learning its driving notes), the racing line and
  centre line, the driver personalities and the driver lounge (`drivers.res`), which cars are near which,
  and the ghost-car viewer's logic. The known AI crash is reproduced as the original has it; the fix comes
  as its own step.
- **3.6, the physics loop and what hangs off it:** the physics task (the tick, the timer that decides
  how many ticks to run, creating and renormalising objects, collisions against the track's static
  objects), the `Physics*` API and the control readings, the cameras, race records and the `.sco` file,
  the game's own replays and the ghost car, and the network car. The race recorder and the shadow check's
  output capture sit in front of the rewrites that share their addresses (`detour_front`).

That is the whole physics and AI code, apart from the debug screens' drawing.

- **The world** (`hook/wld_*.cpp`, 274 more): the terrain (the track's ground-contact
  tree and the unused BSP), loading the track and parsing its world file (cars on the grid, obstacles,
  wobbles, statics, checkpoints), the car list and the game's race set-up (`game.obj`), the car objects
  the renderer draws (models, textures, smoke, dust, skids), and the effects (shadows, smoke puffs,
  reflections, crash debris, sparks, splashes). The drawing itself is left for the graphics stage. The
  world runs every frame on the main thread, so it is checked by shadow-mode races and by replays.
- **The kernel and utilities** (`hook/krn_*.cpp`, 437 more): start-up, memory, threads, the physics
  timer, locks, the crash handler and FPU modes, timing and profiling (`krn_core`); files, the log, the
  rest of `win32.obj`, message pipes and shared memory (`krn_file`); the keyboard and mouse queues,
  random numbers, pools, bags, memory streams, string tables and base64 (`krn_util`); and the resource
  archives, languages and units, the options file and telemetry (`krn_res`). They make the same Windows
  calls as the original, through the game's own import table, and call its C runtime by address, so
  they change nothing yet about what the game runs on. The 3-byte `rdtsc` routine is too short to hook
  and stays original in the game.
- **The graphics** (`hook/gx_*.cpp`, `hook/wld_draw.cpp`, 586 more): the 2D canvas, fonts, palettes,
  bitmaps and the debug dashboards (`gx_2d`); models, lighting, the camera and the deferred surfaces
  (`gx_model`, which now carries M1's texture-bucket lift); the texture cache and texture loading
  (`gx_tex`); the DirectDraw and Direct3D layer, video modes and render state (`gx_dx`); and the world's
  drawing (`wld_draw`). The thin wrappers the game keeps its DirectX objects in (dd.obj, `gx_dd`) call
  the OpenGL renderer (`gl_core`) directly, and so do the draw calls in `gx_dx`. The original code, and
  the race.bin builds, reach the same renderer through a DirectDraw / Direct3D facade (`ddraw_gl`), on
  the same objects, so the game sees the same modes, capabilities and "16 meg card" either way. In a
  shadow check every OpenGL call is recorded (`gl_table`) and compared, with the renderer's state, so
  each frame's drawing is checked against the original's, call by call. `test/world_gx_dd.cpp` checks
  the wrappers offline against the originals on the same renderer.
- **The sound** (`hook/snd_*.cpp`, 264 more): the sound objects, the sound manager (voices, 3D
  priority, the listener) and sound resources (`snd_mgr`); the engine and tyre sounds (`snd_car`); and
  the software mixer, its inner mixing loops and the streaming to the sound card (`snd_mix`). They mix
  on the physics thread, as the original's did. The streaming (wave.obj) writes straight into the SDL
  audio core (`audio_core.h`); the original code and the race.bin builds reach the same core through the
  DirectSound facade (`dsound_sdl`), on the same objects. A shadow check records every call to the core,
  including a hash of the mixed audio written, and compares them. The hardware DirectSound mixer the
  game ships (ds.obj) is never used by any build and isn't ported.
- **What those libraries had left** (`hook/krn_leftover*.cpp`, 2,739 more): the static initialisers
  (`$E`, which set up the game's global constants and objects before `WinMain`), the empty virtual
  stubs, the compiler's deleting destructors and the per-file assert helpers. `tools/gen_leftovers.py`
  reads each from its exact instruction bytes and writes the 2,734 mechanical ones; five are by hand.
  The initialisers run once at start-up, before a shadow check could, so `test/world_leftover.cpp`
  checks every one against its original offline, and a race recorded with them replays identically on
  the original code. Still original: the 13 platform functions the SDL layer replaces, and `WinMain`,
  which comes with the main loop.
- **The widget toolkit** (`hook/ui_*.cpp`, 458 more), the first of the UI stage: the text styles and word
  wrap (`ui_style`), the widget windows (`ui_window`: creating and running them, focus, groups, the title
  bar), the widgets themselves (`ui_widget`: buttons, check boxes, radio buttons, sliders, scroll bars,
  list and drop-down boxes, text input, number fields, the control detector), and the dialogs and menus
  built from them (`ui_dialog`: the OK / yes-no / input / open and save file boxes, `UIDoMenu`,
  `UIDoDialog`), with their static initialisers generated (`ui_leftover.cpp`). Every menu is built from
  them. `test/world_ui.cpp` checks all 458 against the originals offline on real windows made by the
  game's own code; the modal ones, and those that allocate or reach menu code through a callback, are
  checked in game by a session replay.
- **The single-player menus** (`hook/menu_*.cpp`, 1,047 more): the options screens (`menu_options`:
  graphics, sound, controls and the control test, driving aids, hacks), the race menu and race setup
  (`menu_race`), the car, track and opponent choosers and the race-options viewer (`menu_view`), the
  results and high-score boards (`menu_board`), the garage setup editor (`menu_car`: every tab, loading,
  saving and exporting setups), and the static initialisers of all eleven menu files, the multiplayer
  screens' included (`menu_leftover.cpp`, generated by `gen_leftovers.py --lib menu`). The multiplayer
  screens themselves wait for the multiplayer stage. `test/world_menu_car.cpp`, `world_menu_options.cpp`
  and `world_menu_view.cpp` check them against the originals offline; a session visiting every screen
  replays identically on the original code.
- **The race front end and `WinMain`** (`hook/root_*.cpp`, 865 more): the dashboard and HUD, the
  countdown, the escape (pause) menu, the loading, victory, loser and DNF splashes and the hacks
  (`root_dash`, `root_screens`), the main loop, key handling, the blimp and camera helpers, race setup
  and the car and track lookups, the pre-race grid and the post-race results and laps table
  (`root_main`, `root_race`), the replay viewer and its controls (`root_replay`), the ghost cars' files
  and targets (`root_ghost`), `WinMain` itself, and the static initialisers of all fourteen root files
  (`root_leftover.cpp`, generated by `gen_leftovers.py --lib root`). `test/world_root_hud.cpp`,
  `world_root_race.cpp` and `world_root_replay.cpp` check them against the originals offline; a session
  of five races, a restart, a switch-away and a replay replays identically on the original code.
- **The career, the paint kit and the credits** (`hook/career_*.cpp`, `hook/paint_*.cpp`, 789 more): the
  career's slots, menu, events, testing, rankings and end-of-season screens with their saved files
  (`career_main`, `career_screens`), the upgrade shop (`career_shop`), both credits screens and the intro
  video (`career_credits`), the paint kit's canvas, tools, palette, templates, decals, undo and files
  (`paint_kit`, `paint_main`) and its TGA reader and writer (`paint_tga`), and the static initialisers of
  all thirteen of their object files (`career_leftover.cpp`, generated by `gen_leftovers.py --lib career`).
  `test/world_career.cpp`, `world_career_shop.cpp` and `world_paintkit.cpp` check them against the
  originals offline; a career session of two races, the shop and the credits, and a paint-kit session,
  replay identically on the original code (the end-of-season screens are checked offline only so far).

Then a chosen set of the original's bugs is fixed, always on: the AI crash (a car losing its place on
its racing line), degenerate racing lines, a ground-contact divide by zero, vrmod's obstacle wake, a single race on a
track with no AI racing line (it runs without AI cars instead of crashing), sound crashes (Doppler at
the speed of sound, no sound device, mod sounds and engine files), the menus' widget crashes (divides by
zero, text and file-name overruns, overflowing widget and style tables), the menus' own (a mod car with
more than six gears, a random race with two tracks, long car, track and setup names and translations), the race front end's (long mod car and track names
and translations, the command line), the career's and the paint kit's (long mod car and track names, long
translations and user folders, a modded upgrade set or decal table), dialogs over the 3D replay view drawn speckled by the renderer, and
buffer overruns that long mod-car, driver and track names could trigger. A fix only changes what happens
where the original would crash, hang or overrun, so a race replays identically on the original code
until one of those comes up. The AI crash is the one that comes up in ordinary racing (after the
stranded-car teleport), so a recording can part from the original there. They are listed in
`docs/FIXES.md`. Switching away from a single-player race (Alt-Tab) pauses it, where the original's
physics carried on unseen; the sound goes quiet while away, as DirectSound's did. A `race.exe` carrying vrmod's patches
is recognised: the rewrites carry its fixes, do what its features do and read the values it writes (the resolutions, the
hornball), and replace every patched function (`docs/FIXES.md`, "vrmod's patches").

A rewrite replaces only the exact stock v1.0 function it was written from: before anything is patched,
the DLL fingerprints each function's code and the read-only constants it reads (`hook/stock.inc`), and
a function patched by anything but vrmod (as the port knows its patches) stays original, so the patch keeps working.

Each is checked in game in shadow mode, or, where the game never reaches it, offline. A race recorded
with all of them running and replayed on the original code is identical tick for tick, up to a fix
(above), and the same replay in shadow mode matches the original's. How to port a
function this way is written up in `docs/PORTING.md`. The rules that make bit for bit possible are in
`hook/x87.h`:
- every sum keeps the original's grouping;
- a value the original keeps in an x87 register is a double, and one it stores is a float;
- `fsin`, `fcos` and `fpatan`, which precision control doesn't round, never pass through a C variable;
- comparisons keep the original's NaN behaviour;
- constants keep their width, and aren't folded at compile time.

`test/world_*.cpp` are world harnesses. Each loads the original code, builds real cars, wheels and
drivetrains, AI cars, racing lines, replays and network cars in memory, and compares the original against the rewrite on millions of random states.

`tools/gen_port_tables.py` generates what the DLL needs: the original instructions at every hooked
address (for trampolines), the physics classes and their named fields, and the statics of the physics
and AI object files, attributed by which file's code uses each address, with the input code and the
main thread's buffers left out.

## Testing multiplayer on one PC

A test switch for the multiplayer stages, off by default (without it the game is exactly as it was). In
`viperport.ini`:

```ini
[test]
two_copies=1
```

Then start the game twice from the same folder. The second start runs beside the first instead of being
refused by the game's single-instance check, and becomes **copy 2**:

- Copy 2 has its own user folder, `Config-2\`, so the two copies never write the same file. It is made the
  first time, as a copy of `Config\` (or, if there's no `Config\` yet, of the old user folder the way `Config\`
  is made). Its logs (`log.log`, `except.log`, `timer.log` and the DLL's `viperport.log`) go to `log-2\`.
  Copy 1 keeps `Config\`, `log\` and `viperport.log` next to the DLL, as always.
- Both copies open in a window instead of full screen: copy 1 on the left half of the main screen and copy 2
  on the right. Each window is the largest 4:3 or 16:9 picture that fits its half. Copy 2's title is
  "Viper Racing (copy 2)". For the windows you need `[platform] sdl=1` and `renderer=gl`, because DirectDraw
  takes the whole screen.
- A copy keeps running while the other one is in front: the game's switched-away wait is skipped, so a
  network game goes on in both windows. The mouse isn't held inside either window (nor in the screen's top-left
  640x480, where the game's own start-up clip would put it). A single-player race in the
  copy behind still pauses, and the copy behind is silent, as DirectSound made it (only the copy in front is heard).
- A third start while both run is refused, as usual.

Which copy a start becomes is decided by two slots that are held for as long as each copy runs. The first
free slot wins, so if copy 1 is closed, the next start is copy 1 again. The single-instance check and the
folders come from the rewrites of `start_unique_instance`, `get_user_directory`, `log_file_begin`,
`ExceptBegin` and `TimerWatchdog::dump`, so leave those on `new` (the default). Turn `two_copies` off
again before playing normally.

## Layout

- `hook/` — the DLL: `viperport.cpp` (M1 and install), `port.cpp` (M3's rewrites and shadow checks),
  `phys_*.cpp` (the rewritten physics),
  `replay.cpp` (the race recorder), `session.cpp` (the session recorder), `platform.cpp` (window and input), `gl_core.cpp` (the OpenGL
  renderer, every GL call through `gl_table.cpp`; `gl_dxgi.cpp` presents it through a DXGI swap chain on NVIDIA), `ddraw_gl.cpp` (its DirectDraw / Direct3D facade),
  `dsound_sdl.cpp` (the audio core and its DirectSound facade), `ui_*.cpp` (the widget toolkit), `menu_*.cpp` (the menus), `root_*.cpp` (the race front end),
  `career_*.cpp` and `paint_*.cpp` (the career and the paint kit); `build.bat` builds it (Visual Studio Build Tools,
  SDL2 2.32 in `../sdl2`).
- `tools/` — the analysis: linker-map extraction, the function inventory, cross-build matching
  (`match_builds.py`, `propagate.py`, `port_sites.py`), type recovery for Ghidra (`recover_types.py`,
  `type_names.py`, `names/`, `ApplyTypes.java`), generators (`gen_com_base.py`, `gen_texture_table.py`, `gen_port_tables.py`, `gen_leftovers.py`), `trace_diff.py`, `session_diff.py`, and `disasm.py` (disassembly with every constant's value and width).
- `test/` — the offline fuzzer for pure rewrites (`fuzz.cpp`, `fuzz.h`, `build_fuzz.bat`) and a per-file compile check.
