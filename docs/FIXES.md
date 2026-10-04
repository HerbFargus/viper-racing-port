# Fixes

The rewrites reproduce the v1.0 `race.exe` bit for bit, bugs included, and then fix a chosen set of
those bugs. The fixes are always on. Each one changes only what happens where the original would crash,
hang, overrun a buffer or turn a degenerate case into NaN, so a race is identical to the original's, tick
for tick, until one of those cases comes up. In the code each is marked `// FIX:` (see docs/PORTING.md,
"Fixes").

The AI crash below is the one that comes up in ordinary racing: an AI car teleported back onto the track
can lose its place on its racing line (it did within a minute on the Coliseum). On a stock `race.exe`
the original then crashes. On one with vrmod's patch it survives, but puts the car's place at the head of
the line, where the fix puts it at the nearest point. So a race recorded with the rewrites parts from the
original there, in that car's steering target, and a shadow check of `IdealLine::reset_bead_position`
reports it as a mismatch. That is the fix, not a porting error.

## The AI crash

An AI car can lose its place on its racing line (the "bead" left empty). That happens when its position
goes NaN, and after the stranded-car teleport. The original then reads through the empty pointer in a
dozen places and the game dies.

- `IdealLine::reset_bead_position`, where the place goes missing: the car is put back at the nearest
  point of its line. If its position is NaN, it keeps its previous segment, or goes to the head of the
  line.
- Every reader puts a missing place back the same way before using it: `IdealLine::advance_bead`,
  `CenterLine::update`, `update_2d_data`, `get_car_dlong_meters`, `get_car_dlong_cookie`,
  `time_between`, `AICar::check_for_too_fast` and `init_rt_lat`.
- vrmod's AI bead guard (back to the head of the line, in `advance_bead`) is recognised, and replaced by
  this fix, which puts the car at the nearest point of its line instead of the head.

## Racing lines

- `ILSeg::QuickTan`: a segment whose tangent has no length (a two-node line, coincident nodes) takes the
  chord's direction instead of 0/0.
- `advance_bead`, `get_rabbit_position`, `get_nearest_bead`: a zero-length (or NaN-length) node no
  longer makes them loop forever.
- `fixup_res`: a racing-line file with no nodes no longer writes before its own allocation (heap
  corruption) when it is loaded.

## Obstacles

- `Obstacle::Reset` wakes the obstacle (`Obstacle::Perturb`), as vrmod's obstacle-wake patch does: an
  obstacle that had settled no longer hangs 4 m up at its spawn after a restart. A race.exe with vrmod's
  patch is recognised and gets the same.

## Collisions

- `SphereVolume::CollideGround`: a sphere just touching the ground with no sideways speed computed a
  friction limit divided by zero. The physics thread runs with divide-by-zero exceptions on, so that
  crashed the game (about 1 in 13 such contacts). No sideways speed now means no friction.

## Tracks with no AI racing line

- `LoadRace`: a single race on a track with no AI racing line (`default.ili`) crashed as it started. It
  now runs without AI cars, and says so in the log.

## Names and tables (mostly mod cars and tracks)

- `Car::Setup`: a car name over 31 characters is cut to 31 (it overran the car's name field).
- `Wheel::Setup`: a 31-character car name zeroed part of the tyre width while building the tyre file
  name; the name gets its own buffer.
- `AICar::begin_race`, `AICar::stuff_event`: driver names and event text are cut to fit 32 bytes.
- `generate_notes_for` (learn mode): driver and car names are cut to their fields.
- `GetUniqueDSTCMunge`: a mod car's 4-character code is terminated, so ghost and notes file names no
  longer pick up stack garbage.
- `AIDriverLounge::fixup_res`, `copy_res`: at most 16 drivers per strength from `drivers.res`.
- `init_baserecord`: car and driver names are cut to the race record's fields.
- `load_tv_cameras`: at most 32 rows of `camera.tab` (the 33rd overwrote the physics timer).
- `set_best_stages`: stage records outside the table are skipped (they overwrote the stack).
- `RecordMgr`: the records file's path is built in a buffer long enough for any user directory, and a
  long track name is cut to `<first 10>.sco`; a name shorter than 3 characters no longer clears a byte
  before the object.
- `create_phob`: an oversized creation record is read up to the buffer and skipped past; a negative size
  reads only the header.

## Where the game writes

- `get_user_directory`: saves (options, career, records, ghosts, setups, paint) go to `<game folder>\Config\`,
  built from `race.exe`'s own path, not `C:\Program Files\MGI\Viper98\` (the v1.0 exe's literal) or a
  path relative to the current directory (vrmod's writepaths patch). The released `race.bin` and vrmod
  already use `Config\`. The first time the folder is made, the old user directory is copied into it,
  the UAC VirtualStore copy first; the old files are only read. A game folder over 200 characters keeps
  the old behaviour (the game's own limit).
- `log_file_begin`, `ExceptBegin`, `TimerWatchdog::dump`: `log.log`, `except.log` and `timer.log` go to
  `<game folder>\log\` instead of the root of C:, which isn't writable without elevation, so there was
  no log at all (and every race end tried to create `c:\timer.log`). `c:\career.log` is written by the
  career code, which isn't ported yet.

## Files and resources

- `FileReadLine`: a line ending in a bare LF (a file saved on Linux or a Mac) kept all but its last
  character, and an empty first line wrote before the buffer. Only a real `
` is cut now; CRLF files
  read exactly as before.
- Resource names of exactly 16 characters, the most a table of contents holds, were never found (there
  is no terminator to compare against). They are now. That makes 11-character car names work
  (`<car>10.mod` is 16), where 10 was the limit.
- Archive (set) names of 16 or more characters overran their 16-byte field, so later lookups failed and
  the archive was never unloaded; the full name is now kept beside it. (A car can't use one: its
  resource names would pass 16 characters.)
- `ResourceGet`, `ResourceExists`, `hunt_for_resource`: paths of 64 or more characters (32 or more for
  the archive part) overran the stack; they are now simply not found. A directory plus archive name
  too long for its buffer is skipped. Hunted files with long names are freed again.
- `enumerate_language_resources`: a `.lng` file name of 32 or more characters is skipped and a long
  language name cut, instead of overrunning the table.

## Drawing

- Texture names of 16 characters or more overran the texture table's 16-byte name field: a keyed
  16-character name (a car's damaged copy) was never found again, so every use loaded another copy;
  names of 21 or more crashed the first time they were drawn, and names of 24 or more reloaded a garbage
  name after a mode change or a de-rez. The full name is now kept beside the table, so names of any
  length work (`willysjeep11.tex`, 16 characters, is the case that found it); names of 15 or fewer are
  stored exactly as before.
- A texture whose surface couldn't be made crashed when it was drawn, grabbed, blitted or sized; it is now
  treated as no texture.
- Model surface texture names of exactly 16 characters (the whole field, no terminator) ran on into the
  next field: names compared wrongly, every rebuild fetched the textures again, and the texture cache was
  asked for the name with junk on the end. They are read as exactly 16 characters now.
- A missing model (`mrModelInfoGet`) ended the game, and the loaders then read through the empty result.
  It is now logged and loads as an empty model that draws nothing.
- A model with no surfaces crashed when drawn in deferred mode; it now draws nothing.
- Lighting: a normal longer than 1 (mod models can carry unnormalised normals) read past the diffuse
  table, and the specular index had no bound either way; both are clamped to their tables, so in-range
  values are unchanged. `mrLightSpecial` ignores a light number outside 0-7 instead of writing past the
  lights.
- A car with an 11-character name (the longest whose models fit): its skin texture's name, `<car>1.tex`, is
  16 characters, and its terminator landed past the car object's 16-byte field, which the next field then
  overwrote, so the damage texture asked for the name with junk on the end. The name is kept to the field
  and read back as 16 characters.
- `CarObject::Draw2D`: a multiplayer name tag longer than 43 characters is cut to 43 instead of
  overrunning the stack.
- `CarObject::DrawWheel`: a wheel detail level above 2 in a car's `L.tab` draws the lowest-detail wheel
  instead of a garbage model handle.
- `QuarterCarControl::Draw`, the suspension rig's debug dashboard, printed each of its four lines into
  80 bytes, so a large value overran the stack. A damping ratio from a zero or tiny mass or rate can print
  over 300 characters, and the best grip over 100. The line's buffer now holds the longest any of its formats can print,
  and the line is drawn whole, running off the edge as the original drew it. Lines that fitted are unchanged.

- The OpenGL renderer (not the game) laid the 2D over the 3D by what the 2D changed: a pixel of the page that kept
  the colour it was handed (the 3D, shrunk to 640x480) let the full-resolution 3D show. A pixel the 2D drew in exactly
  that colour looked unchanged too, so a dialog over a live 3D view came out speckled with bits of the scene: the
  replay's save and load boxes, whose background (`fdialog.stp`) is almost all near-black, lost 5-20% of their pixels
  over a dark replay view. A run of up to 8 such pixels with changed pixels at both ends, along a row or a column, is
  now drawn as 2D as well (its colour is the shrunk 3D's anyway). Large transparent areas still show the 3D; a small
  gap inside or between 2D elements (the inside of a HUD digit, the cursor's) shows the 3D at 640x480 instead of the
  window's resolution. The page the game draws is unchanged, and so is the session recorder's hash of it, except
  where a later Lock reads back a picture with filled pixels in it: the cursor's grab in the same frame of a dialog
  (the UI's windows run with 3D widgets), and the next frame's Lock where no 3D was drawn over them. Sessions
  recorded before this can part from a replay there, in the 2D page; record them again.
- The same colour test still failed where no 3D was drawn at all: a whole menu screen, the dash band. The picture
  under the 2D there is whatever the screens before left (drawn at the window's resolution), so a black area of a new
  screen kept faint outlines of the last one's widgets wherever their shrunk colour came out black too (the Hacks tab
  after the Controls tab). The renderer now notes where 3D is drawn each frame (a 3D scene's viewport; for
  screen-space triangles, such as the translucent banners, only their bounds; clears don't count, a cleared colour
  shrinks to itself), and everywhere else the 2D page is drawn whole. The colour test is kept only over the 3D. The
  page the game draws is unchanged; sessions recorded before this can part from a replay where a later Lock reads the
  picture back (as above).
- The 2D page was stretched to the window with a plain linear filter, which blurred it (at 4K each game pixel spans
  4.5 screen pixels). It's now drawn sharp: each game pixel is a solid block, blended only across the one screen pixel
  where two blocks meet, as fits the game's 640x480 art. (Display only, `gl_core.cpp`.)

## Switching away

- Alt-Tab during a race: the original stops drawing and reading keys while it's away, but its physics
  runs on its own timer thread, so the race carried on unseen and the player came back to a crashed or
  beaten car. A single-player race now pauses as the Esc menu pauses it (`PhysicsPause`), and unpauses
  on the way back; the unpause resynchronises the physics clock, so nothing is caught up in a burst. A
  network race still runs on, as it must. (`platform.cpp`, `WM_ACTIVATEAPP`.)
- The sound going quiet while away is not a fix: DirectSound silenced an app's buffers while another app
  had the focus, and the emulation does the same.
- The taskbar's preview showed the last menu instead of the race: a full-screen window's frames bypass
  the desktop's compositor, so it kept whatever it last composited. The race's picture is shown once more
  when the game starts waiting, by which time the window is composited. (Cosmetic; `gfx::repaint`.)
- Screenshots (PrintScreen, Win+Shift+S, the Game Bar) and the taskbar's preview showed a stale frame on
  NVIDIA, for the same reason: the OpenGL renderer's full-screen window bypassed the compositor. Where the
  driver offers `WGL_NV_DX_interop2` (NVIDIA), each frame is now handed to Windows through a DXGI flip-model
  swap chain, which the compositor always sees, so screenshots and the preview show the live game. The
  NVIDIA control panel's workaround ("Vulkan/OpenGL present method: Prefer layered on DXGI swapchain") is no
  longer needed; set it back to Auto for the game. The picture, its timing (vsync) and the mouse are
  unchanged. Elsewhere (AMD, Intel, Wine), or if any step fails, the log says so in one line and the game
  presents as before. (Cosmetic; `gl_dxgi.cpp`.)

## Sound

- Doppler: a sound source and the listener closing at exactly the speed of sound divided by zero. The
  sound mixes on the physics thread, which runs with divide-by-zero exceptions on, so the game crashed. That
  pitch is now simply the highest the mixer allows.
- With no working sound device the game went on without a sound manager and crashed at the first sound or at
  the start of a race. It now runs silently.
- The dash sounds (gear changes, cockpit clicks) started out heard or unheard at random, from an
  uninitialised field, and one that started unheard was never heard. Each now starts heard until its first
  update checks it against the listener.
- The sound library could return with its lock still held (a hang), and a sound created for another thread
  was marked done before it was stored, so it could be lost. Both fixed.
- More than 32 fire-and-forget sounds still playing at once ended the game. The extra one is now not played.
- A sound class outside 0-7 wrote past the per-class tables; it is clamped.

Mod cars and sounds:

- Mod sounds without the 4 KB of padding the stock ones carry after their data were read past their end on
  their last mixing block (a crash if the sound sat at the end of its memory). The mixer now stays inside
  each sound's resource and plays what the padding would have held: silence after a one-shot, the loop's
  start after a loop. Every stock sound has that padding and mixes exactly as before.
- A looping sound shorter than one mixing block (about 2,000 samples at the top pitch), or with no data,
  drifted further past its end every block; it now wraps within its loop.
- A missing sound resource makes a silent sound instead of a crash.
- An old-format sound shorter than 2 KB got a negative length and played on through whatever memory followed
  it; it is now silent.
- Engine sound data whose volume corners meet (for example full volume and peak at the same RPM), or a band
  recorded at 0 RPM, divided by zero and crashed the game. The volume is now what the data means, and a
  0 RPM band plays at the mixer's highest pitch.
- An engine file (`<car>e.ens`) with more than 7 bands overwrote the car's idle sound and ran past the engine
  object. The first 7 are used, and the log says so.
- A car with no engine sound data at all had a garbage band count; it now has no engine bands.
- A car using `engine.txt`: a missing `engine<n>.sfx` crashed the game when another car was in view, and one
  engine sound per car was leaked. Both fixed.
- Tyre sounds: running out of memory for one crashed the game, a car index past the 16-car tyre-sound table
  wrote into the mixer's data, and a mixer quality outside the three settings read past the table (it now
  means no tyre squeal).
- Car names of 27-31 characters overflowed the engine file name's buffer (harmless: nothing used the bytes).

## Menus

The widget toolkit every menu and dialog is built from. None of these come up with the stock menus as they
are; most are a mod's text or data (a long track description, an odd setup range), a long install path, or
a menu grown past a table.

- Text fields: a field with no room (a width or line count of 0) took characters without end, writing
  past its buffer; it now takes none. A caret before the start of the text (the field never puts one there)
  wrote the key before the buffer; it goes at the start.
- Number fields (the garage's settings, the options' values) printed into 80 bytes, so a huge value or a
  long format overran the widget; the text is cut to 79 characters. A drop-down list over ~600 pixels wide,
  or with a very long entry, overran the stack drawing its row; the row is now drawn in full, up to 1008
  characters (well past any screen's edge).
- A slider with no steps crashed when dragged (a division by zero); it now behaves as a one-step slider
  always did: dragging sets its value to its one position, the bottom of its range. A list whose rows
  measure 0 pixels high crashed; it shows no rows and a click picks none.
- An arrow button with one step or an empty range, a one-step slider's bars, and a scroll bar on an empty
  list divided by zero. The menus run with that exception masked, so the original got infinities and got
  away with it; the rewrites give exactly the same results without dividing, so nothing changes on screen,
  and nothing can fault if the exception is ever unmasked.
- The open and save file boxes (replays, the model tool): a directory plus pattern longer than the 260-byte
  search path, or a directory plus name longer than the caller's buffer, overran the stack. A directory too
  long to search now lists no files (and says so, as for none found); a name that doesn't fit isn't
  returned (the open box answers as for a missing file, the save box as if cancelled). The name field is
  held to its 300-byte buffer. The boxes also left two pointers aimed at their finished stack frame; they are
  cleared (nothing read them afterwards).
- The text box (the multiplayer prompts: the host to find, the line to connect): a text over 255
  characters, or a limit over 256, overran its buffer; the text is cut to 255 and the field held to 256.
- String lists (the file lists, the multiplayer and setup lists) started with an uninitialised change
  counter. The list boxes only watch it change, so nothing showed; it now starts at 0.
- A title bar dragged while no window was running wrote through a null pointer; it does nothing. A
  picture radio button with no variable did the same when clicked; it does nothing.

Past the toolkit's tables (a mod or menu code that adds too much):

- A style number outside the 32 styles read or wrote past the style table: adding or removing one is
  refused, and drawing or measuring with one uses style 0. A style state past 3 read the next style as a
  palette; it takes the palette of its pressed and over bits. A style stamp with no frames crashed when
  drawn; no frame is drawn.
- Word-wrapped text (the long message boxes, the pre-race track description) had no length limit; it is
  cut to 4095 characters, the size of those buffers.
- A window's 257th widget overwrote its widget count, and a widget's 33rd watched value overwrote its
  value count; both are refused (the widget isn't shown, the value isn't watched). A window's draw keeps to
  its 256 widgets. A 17th window leaked a window before the game stopped with its "too many windows" error;
  the error now comes first. A cycling text button with more than 8 choices wrote past itself; it keeps
  the first 8.

The garage (the setup editor, hook/menu_car.cpp):

- A car name over 27 characters (a mod's) was built into "<car>.car" in 32 bytes with the dialog's item list right
  after it, so the items overwrote the end of the name and the garage unloaded the wrong resource set on the way out;
  past 236 characters "<car>.cf" ran over the garage's return address too. Both names now have 240 bytes, and a car
  name too long for them (no menu can pass one) is cut to its first 235 characters.
- An unsaved setup is saved with a '*' before its name, made by moving the name one place on. A name of 62 characters
  or more (the save dialog takes 63) was moved past the setup, so the setup was saved with a name that doesn't end.
  The '*' and the name now stay inside the name's 64 bytes: a 62-character name comes out as before, a 63-character
  one loses its last character when it's marked unsaved.
- The save dialog copied the setup's name into its field with no limit, so a name that doesn't end (the '*' above, or
  a damaged file) ran over the dialog's return address; it's held to the field's 64 bytes. (Typing a long name was
  always safe: the field holds 63 characters and the original's frame had room for them.)
- A car file with more than 6 gears made the gearbox page (which keeps each gear at least 0.04 below the one before)
  work through that many ratios, so it changed the setup values after the six gears, and then memory past the setup;
  a huge gear count sent it through the game's memory until it crashed. It now keeps the six gears the garage shows.

The main menu and the single race's setup (hook/menu_race.cpp). Neither comes up with the stock game: one needs a
language file's long text, the other a mod's track table.

- The main menu's player name starts as the language file's default (Main_Menu:DefaultPlayerName, "Player" in
  English), copied into a 16-byte buffer with no limit. A translation over 15 characters ran into the menu's item list.
  If a name was already saved it replaced the default and nothing showed. The first time, with no saved name, the item
  list was built over the name's tail and left it unterminated: the name field and the saved option carried item bytes
  after it. A translation of about a thousand characters reached the return address. The default is now cut to its
  first 15 characters. A text that fits is copied exactly as before.
- Random Track picks one of the track table's rows before its last two (the stock table ends with the random entry and
  the unused test track, hell), by the time modulo the track count minus 2. A table of exactly two rows (a mod's table
  with one track and the random entry, say) divided by zero and crashed the game on the way into the race. It now
  takes the first row, as a one-row table always did. Any other table picks as before. (A negative time would give a
  negative row, but the game's clock starts at 0, so that one is left alone.)

The race setup screen's choosers and the high score board (hook/menu_view.cpp, menu_board.cpp). None of these come up
with the stock cars, tracks and English text; they are a mod car or track with a long name or big stats, or a long
translation.

- The track viewer: a track name (tracks.tab) of 75 or more characters overran the stack when "<name>.stp" was built
  for its map. The name is cut to 74 characters. No map has that name, so none is shown, the same as for any missing map.
- The car viewer loads "<car>0.mod" and cuts the name at the character before its first '.' to get "<car>" for the
  paint job's texture. A name of 32 or more characters (a car name of 27 or more) overran the stack. It is now cut to 31
  for the texture names, which are then too long to name a texture, so the car shows unpainted. The model and its .cf
  still load by the whole name, and the .cf's name is held to its 256 bytes. The paint job's texture name
  ("~<car>.tex") overran its 32 bytes. It then went into the model's 16-byte remap entry unbounded, and from 24
  characters on it ran past the entry into an Xlator. The texture name now has room, and the entry keeps to its 0x18
  bytes. The model reads the first 16 characters, as it always did. A name with no '.' wrote to address -1 (a crash),
  and one with a '.' first wrote before its buffer. Neither is cut now.
- The opponent viewer (a ghost car): a car name of 32 or more characters overran its field, over the label and the
  stamps. It keeps 31 characters. Its model name, "<car>1.mod", overran the stack from 27 characters; the buffer now
  holds it. The resource set name ("<car>.car") went into the viewer's 32 bytes. A name too long for them overran into
  the car's name, and a 27-character one overran its texture name onto the return address. The set must be named in
  full to load, so such a car isn't shown, the same as no car. A label translation of 35 or more characters overran the
  stamps' pointers (a crash when they were freed); it keeps 34. MenuDoRaceSetup's ghost car is always "viper", so this is
  only reachable by other callers.
- The car details: the .tab's description (entry 6), at 32 or more characters, overran the car's stats and its car
  list's callbacks; it keeps 31. The stats are printed into texts of 16 and 32 bytes. A big stat or a long translation
  of a unit ran each text into the next, and the last one into the car viewer's vtable (a crash). Each text now keeps
  what fits: 15 or 31 characters.
- The race options: the summary ("<field>: <laps> <unit>", or the event's name) went into the setup screen's 32-byte
  text, and a longer one ran into the option viewer's vtable. The laps text went into 36 bytes, and a longer one ran
  into the opponent viewer. Each keeps what fits, 31 and 35 characters. The setup screen may be the original's, so the
  summary is held to its 32 bytes.
- The high score board: its title ("<race type>: <realism>") went into 64 bytes at the end of the frame, and a longer
  one ran onto the return address. It keeps 63 characters.
- The board's title, and the post-race board's "<race type> : <track>: <realism>", were drawn with the text as a printf
  format, with nothing else passed. A '%' conversion or a '*' in a translation or track name read arguments from the
  stack: garbage, or a crash for "%s" and "%n". Such a title is now drawn as written. Every other title is still the
  format, so it draws exactly as before ("%%" as '%'). The check is the game's own printf state machine, run from its
  table, and it agrees with the game's printf on every test text and 20,000 random ones.

## Race front end

The dashboard and the screens before and after a race (hook/root_dash.cpp, root_race.cpp, root_main.cpp). None of these
come up with the stock cars, tracks and English text; they are a mod car or track with a long name or text, a long
translation, or the command line.

- The dashboard's banners (the reset and damaged messages, the race summary's two headings) were drawn with their
  translation as the printf format and nothing else passed. A '%' conversion or a '*' in a language file's text read
  arguments from the stack: garbage, or a crash for "%s" and "%n". Such a text is now drawn as written. Every other text is
  still the format, so it draws exactly as before ("%%" as '%'). The check is the game's own printf state machine, as for
  the high score board's title, and it agrees with the game's printf on every test text and 20,000 random ones (a "%B",
  which prints garbage without reading an argument, is drawn as written too).
- The drag events' names ("<event> <distance>" twice, "<distance> <unit>", in the locale's units) were formatted into three
  32-byte texts. A long enough translation ran the first two into the translations after them, and the third into the car
  list's pointer, so the car choosers then read the car names from wherever the text pointed. Each keeps its first 31
  characters.
- The car list held 32 cars of 31 characters, neither checked. A longer name ran into the next car's entry (both
  names came out wrong), and a 33rd car ran off the end of the list's memory. The list now grows as the Cars folder is
  read, so every car is in it however many there are. A name over 31 characters is still left out, so the Hacks
  screen's Vehicle list doesn't show it (cut, its name would no longer find its files; every screen that shows a car's
  name is built for 31 characters at most). Up to 32 cars nothing changes.
- The Hacks screen's Vehicle list (Options, Hacks: the only place to change cars) had no scroll bar. It showed its first
  13 cars (17 with vrmod's taller list), in alphabetical order, and any car after them couldn't be chosen. It now has a
  scroll bar down its right edge, inside the list's old outline, so nothing else on the screen moves; drag it, click its
  arrows, or drag a selection past the list's top or bottom. The list opens scrolled to the chosen car. A race.exe with
  vrmod's car-list patch (the list moved up and made taller) is recognised: the list and its bar go where the patch puts
  them.
- The starting grid built each car's model name ("<car>3.mod") and its paint job's texture name ("~<car>.tex") in 32 bytes
  each, so a car name of 27 or more characters overran the stack; both have room now. It then wrote "<car>.tex" and the
  texture's name into the model's remap entry, 16 bytes each, with no limit: from a car name of 12 characters (11 for
  "~<car>.tex") they ran on over the entry and the grid slot's other fields. The original wrote those fields again
  afterwards, so the model got each name's first 16 characters; the grid now writes just those 16, the same names, and
  nothing past them.
- The pre-race screen copied the track's name and its friendly name (a mod track's, or a translation) into 64-byte buffers
  with no limit, a friendly name of 64 or more characters running into the translation after it. Each keeps its first 63
  characters. (It also word-wraps the track's description into 4 KB of its stack and cuts it to 255 characters; that was
  always safe, and a description past 4 KB is cut by the word wrap's own fix, under "Menus".)

The command line (the programmers' switches; the game as installed is started with none of these):

- -d<dir> changes to the directory up to the next space, cut there by writing a terminator where the space was found. With
  no space after it (-d last on the command line) that write went to address 0 and the game crashed at start-up. The
  directory now runs to the end of the command line. With a space, as before.
- -location "( <track> <frame> )" (the blimp test mode) read the track's name into 32 bytes and the frame's base64 into 256
  bytes of the stack with no limit, then copied the track into the race's 32-byte field. A longer track name is cut to 31
  characters; a frame of 256 or more characters (a frame is 32) isn't read, and the blimp starts where it does for a track
  given no frame. The words are measured as the game's sscanf reads them (checked against it on 5,000 random command
  lines); a command line whose words fit is read by sscanf as before.

## Career

The career, its screens and the upgrade shop (hook/career_main.cpp, career_screens.cpp, career_shop.cpp). None of these
come up with the stock game, English text and the game in a normal folder; they are a long translation, a mod track's long
name, a modded upgrade set (viper.ugs) or a very long user directory.

- The career files: "<user directory>career\career<slot>.dat" was built in a 264-byte buffer with no limit, so a user
  directory of 246 or more characters ran it into the translations after it. Cut, the name would no longer be the
  career's file (and past MAX_PATH no file can be opened by it anyway), so such a name is given as empty: the slots can't be
  read (the chooser lists them as empty), made or deleted. Saving then fails the way any save the game can't make does: it
  logs "Can't create career file" and the "saved" box still shows. Saving also built "<user directory>career" in 260 bytes of
  its stack; a directory of 254 or more characters isn't made. Neither comes up with the port's user directory (race.exe's
  folder's Config\, used only when it's 200 characters or less) or the original's short literal; they guard a patched one.
- The class's name (Career:ClassName's translation) was copied into a 48-byte static with no limit. A translation of 48 or
  more characters ran into the copy of the career the game keeps to tell whether it has changed, so leaving the career's
  menu asked to save a career that hadn't changed; about 1,700 characters ran on into the translations after it. It keeps
  47 characters. Nothing shows that copy (the screens translate the class themselves) and it isn't saved, so nothing else
  changes.
- The career screens' week line ("<Week> <n>") was formatted into 64 bytes. A translation of about 60 characters ran over the
  funds line, so the screens showed the week's tail where the funds go, then over the season's, then out of the control
  into the screen's stack. It keeps 63 characters.
- The new career's dialog starts the name from Career:DefaultPlayerName's translation, copied into 16 bytes with no limit.
  A translation over 15 characters, with no player name saved to replace it, left the name running into the dialog's own
  data: the name field showed stray characters after it and Create saved them into the career's name (about 600
  characters reached the return address). The default is cut to 15 characters, as the main menu's is.
- Testing: Test copied the track's name (tracks.tab's, from its first eight rows) into 32 bytes of the caller's stack with no
  limit, so a name of 32 or more characters ran over its return address; the practice race then copied it into the race's
  32-byte track the same way. Cut, the name would no longer find the track's files, and no race can hold it, so Test on such
  a track leaves the testing menu as Back does, and the practice race, given one, doesn't start (the testing menu comes
  back). A mod that replaces a stock track keeps the stock name, so this needs an edited table.
- The upgrade shop, with a modded or damaged viper.ugs: each upgrade's translation keys ("Upgrades:<name>:Name" and
  ":Desc") were built in 256 bytes of the stack, so a name of 242 or more characters overran it. The name is cut to 241
  characters for its keys, which then match no translation, so the shop shows the upgrade as "?!?", as it shows any
  untranslated one. The two tables of translations hold 256 upgrades, and a longer set wrote past them over the shop's
  colours and translations; only the first 256 are translated. (The career's record of what's bought holds 256 as well,
  so a longer set still doesn't work in the shop.)
- The shop's "requires" box ("<required upgrade> <message>", two translations) was built in 256 bytes of the stack, and
  over 255 characters it overran them. The message keeps its first 255 characters.
- The testing menu's picture of the selected track ("<track>.stp") was named in 80 bytes of the stack, so a track name of
  76 or more characters ran over its return address. Cut, the name would no longer be the track's picture, so such a
  picture isn't loaded: the menu shows none for that track, as for any track without one. (The track viewer, under
  "Menus", cuts the name instead and finds no picture; the player sees the same.)
- The career screens' numbered texts: the season's and week's lines of the events screen ("<Season> <n>", "<Week> <n>"),
  the calendar's laps and points ("<n> <Laps>", "<n> <Pts>") and the standings' points were each formatted into the last
  256 bytes of their screen's or control's stack, so a translation of about 250 characters ran over the return address.
  Each keeps its first 255 characters.
- The standings are sorted by points, then results, then the drivers' names, each name copied into 256 bytes of the stack
  first. A name of 256 or more characters (a modded drivers.res) ran over the other name and then the return address.
  Such a name is now compared where it is, which gives the order the copy would have given.
- The upgrade shop: its copy of Upgrade:Purchased's translation went into 256 bytes of the stack with no limit, running
  over the upgrades' snapshot after it from 256 characters and over the return address from 512; it keeps 255 characters
  (nothing reads it). The car summary's engine lines ("<Power> <n> <unit> @ <n> <rpm>", the torque's) and weight line were
  formatted into 80 bytes with the car's figures right after them. A longer line (translations of about 65 characters in
  all, or a car file with absurd figures: a float prints up to 39 digits) ran over those figures, so the torque and weight
  shown were read back out of the text, and a long enough one over the return address. Each line keeps its first 79
  characters, and the figures are the car's.
- The upgrade catalogue word-wraps each upgrade's description (its translation) into a text buffer that was 2 KB, and the
  word wrap copies the whole description (up to 4095 characters since its own fix, under "Menus"), so a description of
  2-4 KB ran over the stack. The buffer is now 4 KB, so a description is wrapped and shown whole up to 4095 characters.
  The item's other lines (the price, the class it needs, available or purchased, what it requires: translations) are
  built in the same buffer and keep their first 4095 characters.

## Paint kit

The paint kit (hook/paint_kit.cpp, paint_main.cpp). None of these come up with the stock cars, English text and a game
folder of ordinary length; they are a mod car with a long name, a long translation, or a user directory far longer than
the DLL makes (it is at most 200 characters, `<game folder>\Config\`).

- The paint kit keeps the car's name in a 32-byte static, and names every file it opens or writes from it ("<car>.car",
  "<car>.tex", "<car>0.mod", "paint\<car>.cvs", "<car>.cvs"). A name of 32 or more characters overran the static, over the
  brush number and the paint kit's other statics; cut, it would name none of the car's files, and the painting would be
  saved under another car's name. The car list holds names of 31 at most (see "Race front end"), so no menu passes a
  longer one; if another caller does, the paint kit isn't opened (the log says why) and the caller carries on as after the
  paint kit closed. The static itself keeps at most 31 characters. A name that fits opens the paint kit as before.
- Default (restore the car's own painting, and the painting shown when the player has saved none) built "<car>.cvs" in 32
  bytes of its stack, so a car name of 28 to 31 characters overran its return address. The name has room now and is used
  whole, so such a car gets its own default painting. A name too long even for that (only a damaged static) is treated as
  a car with no default painting: the painting is cleared.
- At start-up the paint kit makes a texture ("~<car>.tex") for each car that has a painting and no texture. It built
  "<car>.cvs" and "~<car>.tex" in 32 bytes each, one after the other, so a car name of 27 to 31 characters ran each name
  into the next: the texture's name overwrote the end of the painting's, and the painting was fetched by a name that ran on
  into the texture's. Both names are now built whole, and such a car gets its texture. A car list name longer than 58
  characters (only a damaged list) is skipped, as a car with no painting is.
- The paths under the user directory ("paint\<car>.cvs" or "paint\paint<n>.cvs" read and saved, "<car>.tex" or
  "paint<n>.tex" saved and installed, "paint\paint.tga" exported and imported) were built in 260-byte buffers with no
  limit, so a user directory of about 240 characters overran the stack. A path too long for its buffer is no longer built,
  and each use fails the way it already fails when a file can't be opened. The painting isn't read, so the car's default
  painting is shown. The save stops with "Can't create <path>" in the log (the path cut to what a log line holds), and the
  painting is still marked changed (Back, answered Yes, then leaves as after any failed save). The paint folder isn't made
  if its own path is too long. The start-up textures aren't written and are logged the same way. Export and import show
  their error boxes. A path that fits is built as before.
- The decal picker's title is the set's name (a translation), copied into the picker's 256 bytes with no limit. A
  translation of 256 or more characters ran over the picker's own fields and on past it. The title keeps its first 255
  characters.
- The decal picker looks each set's name up as "Paintkit:DecalSet:<name>" (the name from decals.tab), built in 256 bytes
  of its stack with no limit. A decals.tab name of 238 or more characters (a mod's) ran over the picker. Such a name is
  cut to its first 216 characters. No language file has the cut key, so the set's title shows the game's "?!?" for a
  missing translation, and the "Can't XLAT" warning with the key still fits a log line. A name that fits is looked up as
  before.

## Limits lifted

Like M1's limits, these move a table into the DLL for the original code and the rewrites alike, so
nothing changes below the old limit (`viperport.log` says "lifted ...").

- Open files 32 -> 256. Every loaded archive keeps its file open, so this is the archive limit: the
  33rd panicked. (`alloc_file` left original stops at 127, the most its one-byte compare holds.)
- Options 256 -> 4096 (past the end the table overran); a full table now hands out a spare item that
  isn't saved.
- Language files 8 -> 64 (a 9th overwrote the language count).
- A track's camera-facing and upright models in view (trees, signs): 512 and 514 -> 8192 each. More than
  512 overwrote the dynamic-model table, more than 514 the other list. Past 8192 the rest aren't drawn,
  and the log says so once.
- Sounds: the sound manager's table 384 -> 1024 (the 385th overran the heap) and the software mixer's list
  256 -> 1024 (the 257th panicked "full"). Past 1024 a new sound is silent.

## Not fixed

- A track with no AI racing line still crashes in a career race or a multiplayer race; those are fixed
  with their own stages (the menus and career, multiplayer).
- The AI's quirks that shape how it drives (its per-segment speed notes are computed and then
  overwritten) are gameplay, not bugs, and stay.
- `collide_sphere_sphere`'s 0/0 for coincident centres needs no fix: it is caught before its result is
  used.
- Sound: the mixer ignores each sound's sample rate, so the three 44.1 kHz stock sounds (the Viper's engine)
  play as the game always played them; fixing it would change how the stock game sounds. Out-of-range mixer
  and quality lookups are only reachable from the game's own menus. An old-format sound is shortened by 2 KB
  every time it is fetched (no stock or known mod sound is old-format).
- Menus: a menu's custom controls (the options panels, the car and track choosers, the garage, the career
  and paint screens) get their width and height where the toolkit's own layout expects a right and bottom
  edge. Every control reads them as a width and height, so it's as the game was written, and stays. The
  upgrade catalogue's own buffer, 2 KB in the original, is grown to 4 KB by the career's rewrite (under
  "Career"), so every word-wrapped text now fits its buffer.
- Menus, names no stock or known mod file has: the garage builds its setup files' paths in 256 bytes, and
  the car viewer names a car's resource set ("<car>.car") in 32, so a car name of about 235 characters,
  or of 28 to 31 for the viewer, still overruns; the choosers copy a car's or track's friendly name into
  the setup screen's buffers unbounded. A damaged file or options file (a setup slot outside 0..7, a
  negative saved track, an unknown opponent type or mixer, a gear ratio of 1e28), and the menus that set
  a dialog's items once from their first call's stack frame (always the same depth), are left as the
  game has them.
- Race front end, what a player doesn't meet (damaged files, corrupt option values, network races, values the game never
  makes): the option names' and tracks' difficulty tables are indexed unchecked (an option out of range, the "no track"
  -1); the pre-race screen loads the player's setup into the entry before the cars when no car is the player's (a race of
  AI cars only); the post-race screens read the first and the player's race result without checking there is one, and
  step through the cars dividing by their count (0); the race's results hold 16 cars, unchecked; a replay or ghost file's
  track name, packet count and version are trusted (a damaged file overruns the stack or the ghost), and a network race
  with two cars repeated numbers its ghosts past the table; the dashboard formats lap times into 64 bytes (a time over
  1e25) and indexes the gear names by the gear (-1..7) unchecked; the escape menus' selection, a splash's timeout, a
  replay graph narrower than 4 pixels and clear_rect's rectangle are only ever given values in range. All are as the game
  has them.
- Career, what a player doesn't meet (damaged career or season files, a missing season, values the game never makes): the
  class indexes the awards, and the driver map and the event index the drivers' results and the race's cars, all
  unchecked (a damaged career file, a season of more than 32 events); the season's calendar indexes its purses and place
  names by a place from the file unchecked; a race's winnings read the entry before the results when no car is the player's
  (the driver map always holds the player); a season that can't be loaded is read through 0; the career log formats into
  1 KB (its longest line is far shorter); an upgrade page holds 64 items (the shop's own lists have 4 to 10), and a right
  click in test mode reads an item's upgrade before checking the item (the control's rectangle keeps it in range). Left
  as harmless: each career screen adds its status control before formatting its texts, so for a moment its lines copy
  whatever the stack held (the first update replaces them before anything is drawn); the chooser lists slot 1 twice (a
  second button on the first); the testing menu's list starts with an uninitialised change count (the list box only
  watches it change).
- Paint kit, what a player doesn't meet (damaged files, values the buttons never set, out of memory): a tool number outside
  0..8 (a panic, then a call through a null tool) and a brush number outside 0..4 (read past the brush table) are only set
  in range by the radio buttons and the selection tool; the templates are indexed by the template shown unchecked (no
  templates found); decals.tab is read with no bound on its rows (65 or more overrun the picker) and a set of 0 decals, or
  decals 0 wide or high, divide by zero; the set shown is kept from the last time and not checked against the sets read (a
  shorter decals.tab); the TGA export and import don't check their buffer's allocation and take a canvas's rows as one run
  (every canvas the paint kit gives them is). The brush line is offset by half the brush's width, not its height; the
  brushes are square, so it draws as the game always drew. All are as the game has them.
