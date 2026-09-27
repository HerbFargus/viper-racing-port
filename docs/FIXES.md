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

## Not fixed

- A track with no AI racing line still crashes in a career race or a multiplayer race; those are fixed
  with their own stages (the menus and career, multiplayer).
- The AI's quirks that shape how it drives (its per-segment speed notes are computed and then
  overwritten) are gameplay, not bugs, and stay.
- `collide_sphere_sphere`'s 0/0 for coincident centres needs no fix: it is caught before its result is
  used.
