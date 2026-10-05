# Porting a game function, bit for bit

How M3 rewrites the v1.0 `race.exe`'s functions so they match the originals exactly, and how each
rewrite is checked. Read `hook/x87.h` and `hook/phys_math.cpp` (a finished example) alongside this.

## Why bit for bit is possible

The physics thread sets the x87 FPU to single precision every tick (`physics_thread` →
`ExceptSinglePrecision(1)`): every operation rounds its result to a float's 24-bit mantissa, while the
registers keep their full 15-bit exponent. This DLL is built `/arch:IA32 /fp:precise`, so its own float
arithmetic runs on the same x87 in the same mode. A rewrite that does the same operations, in the same
grouping, with the same constants, on values of the same range, gets the same bits.

## Materials

- `python tools\disasm.py 0xADDR ...` — the exact disassembly: calls named, every memory constant shown
  with its value and **width** (float or double). The ground truth; port from it.
- `tools\ghidra_decompile.bat 0xADDR ...` → `out\decomp_sample.c` — Ghidra's decompilation with the
  recovered types. Good for structure; it reorders arithmetic and hides precision, so never trust its
  order of operations.
- `out\types.json`, `out\types.tsv` — recovered class layouts and field names
  (`grep "^field\tWheel\t" out\types.tsv`).
- `hook\phys_types.h` — shared layouts with `static_assert` offsets, and `VFN()` for virtual calls.
- `out\symbols.csv` — every symbol's address, demangled signature, library and object file.

## The rules

1. **Same grouping.** Two-operand `+` and `*` are commutative bit for bit; `(a+b)+c` is not `a+(b+c)`.
   Read the grouping from the `fld`/`fadd`/`faddp` sequence, not from the decompiler.
2. **Registers versus memory.** A value the original keeps in an x87 register is a `double` in the
   rewrite (the register's exponent range: a sum of squares past 3.4e38 still has a square root; a
   product below 1e-38 isn't zero); a value it stores with `fstp dword` is a `float`. Cast to `double`
   for intermediates and to `float` exactly where the original stores. A function whose original
   returns an unrounded ST0 returns `double` (the same register to its callers). Trace the x87 stack
   through `fxch`, `fld st(n)`, `fstp st(n)`: a value can live in a register for a long stretch.
3. **Constants keep their width and exact value.** `fmul dword ptr [c]` is a float constant (`0.5f`),
   `qword` a double. Write odd values from their bits (e.g. `0x3d666666` is not `0.05625f`).
4. **Comparisons keep the original's NaN result.** The 1998 compiler tests the FPU's flags directly:
   after `fcomp b`, `test ah,1` is "less, or unordered" → `!(a >= b)`; `test ah,0x41` → `!(a > b)`;
   `test ah,0x40` → `!(a < b || a > b)`. An integer compare of a float's bits (`cmp dword [x], ...`) is
   done on the bits.
5. **`__adjust_fdiv`** (`cmp [__adjust_fdiv],0; jne` → `__adj_fdiv_r`) is the Pentium FDIV-bug
   workaround: a plain division.
6. **Bit copies stay bit copies.** Where the original moves a float with integer instructions (`mov`,
   `rep movsd`, a float argument pushed straight on to a callee), copy the bits — `memcpy`, `uint32_t`,
   or a typedef whose parameter is `uint32_t`. A copy through the FPU quietens a signalling NaN. Check
   the compiler's output (`dumpbin /disasm` on your `.obj`) where it matters.
7. **`call __ftol` is NOT C's `(int)x`.** The 1998 routine truncates through `fistp qword` and keeps the low
   dword, so NaN or an out-of-range value gives 0; today's `(int)` can take an SSE path that gives
   0x80000000. Use `x87_ftol` (x87.h).
7a. **No compile-time folding of what the original computes at run time.** An expression of constants
    (`900.0f * 80.0f`, `1.0f / (4180.0f * 12.8f)`) is folded by the compiler in double precision; the
    original computes it at run time on the x87 in single precision, and the results can differ. Where
    the original does arithmetic on two constants at run time and the result isn't exact in 24 bits, load
    them through `volatile` (or write the exact bits the original produces).
7b. **Narrow integers can compile differently.** A 16-bit bit operation written in C
    (`mask |= (uint16_t)(1u << n)`) can come out as `bts cx, ax`, which takes the bit number mod 16,
    where the original's `shl ax, cl` shifts an out-of-range bit away entirely. Do the arithmetic in 32
    bits and truncate at the end, and check the compiler's output where the original works on 8 or 16 bits.
8. **Calls.** Call other game functions by their v1.0 address with an exact typedef; a `__thiscall` is
   called and received as `__fastcall(self, void* edx, args...)`. Virtual calls go through the object's
   vtable (`VFN(obj, byte_offset, Ret, Args...)(obj, 0, args...)`), never directly, so the callee's own
   rewrite — hooked at its address — is what runs. Keep every call, in order, with the same arguments,
   and read memory at the same points relative to calls.
9. **x87-only instructions.** Precision control rounds `fsqrt` (so `x87_sqrt` returns a double that
   holds it exactly) but NOT `fsin`, `fcos` or `fpatan`: they return a full 64-bit mantissa, which no C
   type holds. So their result never passes through a C variable: `x87_sin_f`/`x87_cos_f` store it
   straight to a float, `x87_sin_mul`/`x87_cos_mul` multiply it in the same asm block (the product is
   rounded); any other sequence, and `fpatan`, `fyl2x`, `frndint`..., gets a small asm helper in your
   file that does exactly what the original does with the result. The fuzzer can't catch a mistake here
   (it changes about one result in 5e8): it has to be right by construction.
10. **Faithful first.** Reproduce the original's bugs and quirks exactly (a 0/0, a missing bounds check,
    dead stores that matter). Fixes come later, as their own step.
10a. **Stores the result never needs still happen.** The physics thread runs with overflow and
    divide-by-zero exceptions ON (`timer_vector` → `ExceptDiv0Crashes(1)`), so an `fst dword` whose value
    is never read again still faults when it overflows. The compiler drops such a store from the rewrite,
    which then carries on where the original stops. Force every store the original makes through a
    `volatile float` (e.g. a small `st()` helper), including a value stored just before an early return.
    Harnesses should also run with those exceptions unmasked to see this.
12. **M1's lifted limits live in the original's code.** M1 (`viperport.cpp`) raises limits by patching
    operands in place: the world and graphics object lists, the physics-object list's size, the model
    and texture pools, the texture table. A rewrite of a function M1 patches reads each such value from
    the original's instruction with `m1_operand(operand address)`, so it gets M1's value in the game and
    the stock one in a harness. `tools/gen_port_tables.py` refuses to run if one is missed.
11. **Dead branches** the original can never take (a compiler alias check comparing two of its own stack
    locals) may be left out; say so in a comment.

## Registering a rewrite

```cpp
static void __fastcall Wheel_UpdateHeat(Wheel* self, void* edx, float dt) { ... }
static void fp_wheel_update_heat(Footprint& f, Wheel* self, void*, float) { f.add(self, sizeof(Wheel), "wheel"); }
PORT_FN(0x0044a440, "Wheel::UpdateHeat", Wheel_UpdateHeat, fp_wheel_update_heat)
```

The name is the map's (the `viperport.ini [port]` key); overloads get a suffix, e.g. `"MatrixSet(rows)"`.
Rewrite function names are file-local but should still be descriptive. The footprint function takes the
rewrite's parameters after a `Footprint&` and lists everything the function may write, besides the
physics and AI statics (saved automatically):

- `f.object(ptr, "what")` — a game object with a vtable, sized by its class;
- `f.add(ptr, bytes, "what")` — anything else (an embedded part, an output, a constructor's `this`,
  whose vtable isn't set yet);
- `f.pure = true` — reads and writes no globals (also fuzzed offline). It doesn't cover the object: a
  member function that writes `this` still lists it, or the check runs the rewrite on the original's
  result and never compares it. A setter that steps a value (`Car::SetSteering` moves at most 0.08 a
  tick) then steps twice, and the race parts from the original in shadow mode alone;
- `f.replay_only = "why"` — allocates, frees, loads files, or writes what can't be bounded: checked by
  whole-race replays, not shadow checks.

**Threads.** The physics runs on the game's timer thread; the input code (`DriverUpdate`,
`ControlUpdate`, `PhysicsReadControls`) and everything else on the main thread. A shadow check only
saves and restores the physics and AI statics when it runs on the physics thread; a function that runs
on the main thread must list the statics it writes in its footprint (`f.add(address, bytes, "what")`).

**DirectDraw and Direct3D.** Graphics code calls DirectX through the game's interface pointers, which land
in the DLL's emulation (`hook/ddraw_gl.cpp`). During a check, every such call is recorded and the two
passes' records compared like any other output. The original's pass runs the calls live and queues what
each hands back (return values, out-parameters, new objects, and the pixels a `Lock` exposes); the
rewrite's pass makes no call at all and is handed the queued results in order, so it sees exactly what the
original saw and nothing is drawn twice. A footprint lists memory only. The pixels of a surface a function
locks itself need no footprint: both passes' Locks hand out the same pixels, and each Unlock's record
carries the pixels as that pass left them. Arguments that point at the caller's structures are recorded by
what they hold, never by address (the two passes' stack frames differ), and a draw's vertices are recorded
without what Direct3D never reads (an `LVERTEX`'s reserved dword; u/v with no texture bound), which the game
leaves as stack garbage. A function that leaves a global aimed at its own dead stack frame (a local canvas
made current and never unset) lists it with `f.stack_ptr(address, "what")`: it matches when both passes
leave the same value or both an address on the stack. Enumerations call back into the game, so a function
that enumerates is `replay_only`.

Crash sounds and the game's own replay events are outputs, captured automatically. Random numbers and
the controls are inputs, fed to the rewrite automatically, and so is the physics clock (`PhysicsGetTime`)
for a check on the main thread, where the physics can tick between the two passes.

## Checking

1. **Compile:** `test\check_compile.bat hook\phys_x.cpp %TEMP%\somewhere` (warning-free).
2. **Pure functions:** `test\build_fuzz.bat %TEMP%\fz hook\phys_x.cpp` then `%TEMP%\fz\fuzz.exe 1000000`.
3. **Everything else, offline:** a world harness. Load `out\race_v10.exe` at 0x400000 the way
   `test\fuzz.cpp` does (a child process with the range reserved before its heap exists), build real
   objects in memory (vtables are the v1.0 addresses; `hook\state_layout.inc` lists them), patch the
   callees you want to record with a jump to a stub, and compare the original against the rewrite on
   random worlds — object state, outputs and the stubs' call logs. Put it in `test\world_<name>.cpp`.
4. **In game:** shadow mode (`[port] default=shadow`) and a replay with the rewrite live
   (`default=new`); see README.md. A shadow-mode replay runs the original, so its trace must match the
   same recording replayed with `default=original`, tick for tick; if it doesn't, a check leaks (a
   footprint misses something its function writes), and `dump_ticks` at the first differing tick names
   the field.
5. **Main-thread code the races don't reach (the menus):** a session (`[session] record=1`) recorded
   with the rewrite live (`default=new`), then replayed with `default=original`: it compares every frame
   and must report "IDENTICAL over all N frames compared" with 0 reads fallen back. Play through what
   the rewrite runs (the screens and dialogs that call it). This is the in-game check for `replay_only`
   functions that only run in the menus.
6. **The regression corpus:** `python tools/corpus.py` replays every session listed in `tools/corpus.json`
   (names only; the recordings stay in the install) on each of its routes -- `dll` (the rewrites),
   `original` (MGI's code), `standalone` (viperport.exe) -- unattended, and reports PASS / FAIL per run
   with the first difference. It sets and restores viperport.ini, never touches a game it didn't start,
   and keeps each run's log beside its session. Any build change (the relink stages above all) must
   leave the whole corpus PASS. `--list`, `--dry-run`, `--only`, `--routes`, `--selftest`.

## Fixes

Faithful first: every function is ported and verified with the original's bugs intact. Then a chosen
set of bugs is fixed, always on for players and never a switch. A fix departs from the original only
where the original would crash, hang, overrun a buffer or turn a degenerate case into NaN, so every
other input still gives the original's bits, and a recorded race replays identically on the original
code until one of those cases comes up (docs/FIXES.md).

- Mark each one `// FIX: what and why`, and write it `if (VP_FIX && bad case) { ... }` (or
  `VP_FIX ? fixed : original`). `VP_FIX` is 1 in the DLL; a harness that defines `VP_FAITHFUL` before
  including the rewrite gets 0, and checks the original behaviour bit for bit as before.
- Test each fix separately, without `VP_FAITHFUL`: the input that used to fail no longer does, and
  ordinary inputs still match the original.
- The footprint covers what the fixed code writes.
- vrmod's patches to the functions the port rewrites are taken over (docs/FIXES.md, "vrmod's patches"): the
  rewrites carry its fixes or reproduce its features, and the stock check (`tools/gen_port_tables.py`) also
  accepts exactly vrmod's bytes for those functions -- `VRMOD_PATCHES` for fixed patches, `VRMOD_MASKS` for the
  operands where the player chooses a value, `LIVE_CONSTS` for the .rdata floats the hornball tunes -- and
  replaces them. A rewrite reads such a player value from the installed race.exe's own instruction with
  `vrmod_operand(address)` (port.h; `gen_port_tables.py` requires it for every masked operand, and
  `tools/gen_standalone.py` keeps those bytes out of the standalone's int3 fill), so a harness or a stock
  race.exe gets the stock value. `test/vrmod_image.h` turns a harness's image into vrmod's race.exe
  (`VP_VRMOD=1` in the world harnesses that check these). Any other patched function stays original.
