# Fixes

The rewrites reproduce the v1.0 `race.exe` bit for bit, bugs included, and then fix a chosen set of
those bugs. The fixes are always on. Each one changes only what happens where the original would crash,
hang, overrun a buffer or turn a degenerate case into NaN, so an ordinary race is identical to the
original's, tick for tick. In the code each is marked `// FIX:` (see docs/PORTING.md, "Fixes").

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
  this fix.

## Racing lines

- `ILSeg::QuickTan`: a segment whose tangent has no length (a two-node line, coincident nodes) takes the
  chord's direction instead of 0/0.
- `advance_bead`, `get_rabbit_position`, `get_nearest_bead`: a zero-length (or NaN-length) node no
  longer makes them loop forever.

## Obstacles

- `Obstacle::Reset` wakes the obstacle (`Obstacle::Perturb`), as vrmod's obstacle-wake patch does: an
  obstacle that had settled no longer hangs 4 m up at its spawn after a restart. A race.exe with vrmod's
  patch is recognised and gets the same.

## Collisions

- `SphereVolume::CollideGround`: a sphere just touching the ground with no sideways speed computed a
  friction limit divided by zero. The physics thread runs with divide-by-zero exceptions on, so that
  crashed the game (about 1 in 13 such contacts). No sideways speed now means no friction.

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

## Not fixed

- A track with no AI racing line still crashes when AI cars are built (left as the original, for now).
- The AI's quirks that shape how it drives (its per-segment speed notes are computed and then
  overwritten) are gameplay, not bugs, and stay.
- `collide_sphere_sphere`'s 0/0 for coincident centres needs no fix: it is caught before its result is
  used.
