SM64 optimization review — 13 September 2026

Implemented optimizations in the display-list interpreter, GPU submission,
fixed-point matrices and audio backend. The normal RISC-V build and a separate
profiling build are available locally. The existing work in this checkout was
preserved. The vendored SDK and Pocket runtime bitstreams were not changed by
these optimizations. The subsequent MiSTer feature expansion is built separately
in the openfpgaOS repository.

The SS1 became available on 14 September. Hardware comparisons now use the same
expanded MiSTer core at 100 MHz for both software builds, with RGB565 and cached
vertices enabled. The earlier host measurements below describe individual CPU
operations; the hardware measurements at the end describe actual presentations.

Measured results

| Workload | Before | After | Interpretation |
|---|---:|---:|---|
| Cached triangle submission, host median | 20.711 ns | 13.446 ns | 35.1% less host CPU time |
| Fixed-point matrix multiply, RV32 instructions | 367 | 305 | 16.9% fewer executed instructions |
| Same multiply, RV32 loads + stores | 147 | 82 | 44.2% fewer memory operations |
| Same multiply, stack frame | 176 bytes | 112 bytes | 64 fewer bytes |
| Same multiply, machine code | 1,054 bytes | 548 bytes | 48.0% smaller function |
| Mixer active-voice queries, 10,000 synthetic updates | 439,784 | 409,622 | 6.9% fewer service calls |

The host triangle benchmark uses the actual renderer with a mocked GPU sink:
three million submissions per trial, nine interleaved before/after trials.
It excludes FPGA rasterization, SDRAM contention and display cadence. The
ordinary desktop matrix benchmark was essentially unchanged (about 1.5% lower
median); the loop tuning is deliberately enabled only on RV32.

RV32 instruction counts come from executing the actual compiler-generated
`mtx4_mul` disassembly in a small integer interpreter, checking results against
exact arithmetic. Counts include the loop iterations, prologue and epilogue.
They are not cycle counts. Baseline and optimized listings each passed 4,000
cases covering distinct and aliased inputs/outputs.

Changes and rationale

- **GPU state:** all emission paths retain one shared sticky-state cache. A
  cheap check of material invalidation and per-draw alpha, subpixel and combiner
  fields avoids repeatedly constructing and comparing the full state structure.
  Every material setter invalidates this check; cached and fallback triangles
  can still alternate in their original order.
- **Vertex reuse:** only stale GPU slots need CPU attribute preparation.
  Uploads reuse the reciprocal already computed by the vertex loader, and fog
  also reuses that reciprocal. Shader input count and texture usage are cached
  with the combiner rather than queried through the renderer API per triangle.
- **Matrices and lighting:** defer the modelview/projection product until the
  next vertex load, eliminating intermediate products. Prepare light vectors
  outside the vertex loop and calculate texture-generation vectors only when
  texture generation is enabled. Popping a matrix invalidates the affected
  lighting state; a root pop cannot underflow the stack.
- **RV32 fixed-point math:** use local, explicitly wrapping 64-bit dot products
  instead of the software global MAC accumulator. Limit row unrolling on RV32
  to reduce register spills. The arithmetic retains the original final
  truncation and supports aliased matrices.
- **Audio:** pace the virtual audio ring with monotonic microseconds instead
  of an assumed 110 MHz timer. Account for the engine's aligned buffer size so
  the sequencer advances at 60 updates per second. Replace the orphan-voice
  nested ownership scan with a 32-bit ownership mask, preserving mixer output.
- **Profiling/builds:** remove detailed per-triangle timing from normal builds;
  `PROFILE=1` retains it in a separate object directory. Track header dependencies
  and Makefile changes so renderer interface changes rebuild their consumers.

Correctness fixes covered by regressions

- Primitive/environment RGB changes invalidate uploaded vertex colors. LOD
  color depends on the first vertex of each triangle and is uploaded per draw.
- Range-check vertex addresses before deriving a cache slot, avoiding subtraction
  of unrelated pointers for clipped/fallback vertices.
- Texture cache keys include dimensions and palette identity. Palette loads
  invalidate texture bindings, and cache wrap clears stale hash bucket heads.
- The combiner pool wraps within its bounds. Shader overflow uses stable
  allocated entries rather than replacing shader objects still referenced by
  combiners; this also keeps cached shader layout metadata valid.
- In-place billboard construction reads source translation/rotation before
  overwriting the destination.
- Light uploads and matrix pops invalidate cached light directions.

Validation

Two 95,000-triangle synthetic replays produced identical before/after GPU API
payload hashes, including sticky object/surface state and draw order:

| Replay | Hash | Draws | Vertex uploads | Surface-state commands |
|---|---|---:|---:|---:|
| Truecolor, cached/fallback alternation | `10321a6709dbc527` | 95,000 | 26,656 | 9,330 |
| Palettized fallback | `cc8ea536da4bf621` | 95,000 | 0 | 1,000 |

Separate assertions cover changes that intentionally correct the old output:
material color/LOD changes, matrix/light transitions, stack underflow, 20,000
texture insertions with cache wrap, palette/dimension changes, combiner overflow
and 128 additional stable shader objects. Matrix tests exercise 100,000 random
cases and all alias modes, including in-place billboards. Audio tests cover
fractional timing, timer wrap and multiple aligned buffer sizes. The 10,000
voice-update comparison matches mixer output and ownership state exactly.
All these native tests passed AddressSanitizer and UndefinedBehaviorSanitizer.
The fixed-point tests explicitly define integer wraparound.

Both normal and profiling RISC-V builds passed, as did the desktop compile/link
smoke build. The desktop GPU backend is a stub, so that build is not a visual
playtest. The local Pocket SD tree contains the normal optimized ELF.

Reproduce the tests from the repository root:

```sh
python3 tools/check_sm64_optimization.py
python3 tools/check_sm64_optimization.py --sanitize
# Optional: compare against a saved, pre-change src/sm64 tree.
python3 tools/check_sm64_optimization.py --baseline-src /path/to/saved/src/sm64
```

Build commands:

```sh
make -C src/sm64 -j8
make -C src/sm64 -j8 PROFILE=1
# Restore the normal ELF in the local SD tree after building the profiler.
make -C src/sm64
```

Normal ELF: `.obj/sm64/app.elf`.
Profiling ELF: `.obj/sm64-profile/app.elf`.
Local SD tree: `build/pocket/sm64/`.
The shared RISC-V executable uses the existing GPU capability checks on both
Pocket and MiSTer. It was deployed privately and measured on the SS1; the Pocket
build passed local checks but has not received a hardware playtest in this work.

[Raw measurements and provenance](measurements/optimization-20260913/provenance.json)
identify the exact source changes and ELF hashes. The complete working baseline,
build logs and incremental patch are retained locally under
`~/.cache/sm64-optimization-20260913/`. No binary release was published.

SS1 hardware method — 14 September

Private diagnostic builds replay five built-in attract-mode demos with controller
input held at zero. Both versions retain fixed 30 Hz simulation and the original
render catch-up behavior. Each records 4,800 simulation ticks in RAM, then writes
the trace to private save slots after measurement. There is no screenshot, remote
SD-card access or trace streaming during the capture. Sound remains enabled.
Periodic console performance printing is removed from both diagnostic builds.

The trace records physical presentation counts and timestamps, CPU render/audio
cycles, GPU command counts, ring spins and game/camera state. Steady gameplay
statistics consistently omit the first 90 and last 30 simulation ticks of each
demo to separate loading and fades. The full traces retain those transitions.
The minimum FPS statistic is the lowest number of presentations in a complete
sliding one-second window. Each steady interval accounts for every presentation.

The software baseline is a freshly rebuilt copy of the exact source before this
optimization, including its original detailed timing overhead. The normal
optimized build disables that overhead, as documented above. A separate optimized
profiling run retains detailed counters and is excluded from the A/B result.

The old 0.9.5 MiSTer core selects SM64's indexed-color fallback. The shared code
changes produced no meaningful FPS improvement in that configuration. The new
core enables RGB565 and GPU vertex reuse, which changes rendering quality and
memory traffic. Its FPS must be compared with the software baseline on that same
core, rather than presented as a speedup over the old indexed-color renderer.

The expanded core uses fitter seed 30 and reports HW_FEATURES `0x6dffe753`.
Its 100 MHz setup slack is −0.246 ns and hold slack is +0.116 ns across four
corners. Successful SS1 runs do not make it timing-clean or certify every board.
No 90 MHz hardware comparison or audio listening test was performed in this run.

The profiling capture puts vertex processing at roughly 6–10 ms per rendered
frame and texture handling at 3–8 ms across these demos. Legacy triangle emission
takes another 5–13 ms; that counter excludes cached triangle submission, so the
remaining render time cannot be labelled display-list interpretation alone.
Matrix multiplication is now only about 0.2–0.3 ms per frame. Further work should
measure cached submission and rasterizer stalls directly, then evaluate display-
list decoding, fewer vertex uploads and texture conversion/reuse. Moving matrix
math alone into hardware offers limited savings in these measured scenes.

Measured SS1 results

Two baseline and two optimized captures on the expanded core give the following
mean physical presentation rates. Minimums are the lower result across the two
captures for each software version.

| Demo | Before, FPS | After, FPS | Gain | Lowest one-second FPS, before → after |
|---|---:|---:|---:|---:|
| Bowser in the Dark World | 16.85 | 17.66 | 4.82% | 13 → 14 |
| Whomp's Fortress | 15.82 | 16.24 | 2.63% | 13 → 14 |
| Cool, Cool Mountain | 17.22 | 17.57 | 2.04% | 13 → 13 |
| Big Boo's Haunt | 17.32 | 17.55 | 1.33% | 11 → 11 |
| Jolly Roger Bay | 14.16 | 14.39 | 1.64% | 10 → 10 |

Every one of the 4,800 recorded simulation/camera state vectors matches the
fresh pre-change baseline in all eight captures. This covers position, action,
level, area, demo state and camera coordinates, not all internal engine memory
or a pixel-by-pixel hardware comparison. The latter is covered for synthetic
submission sequences by the local regression tests described above.

[Raw hardware traces and comparisons](measurements/optimization-20260913/ss1-20260914/comparison.json)
include the diagnostic ELF/image hashes and a parser for the compressed save-slot
data. Diagnostic ELFs predate the subsequent save-file alias fix below; they use
fresh, private save images for every capture.

MiSTer save portability

`ultra_reimplementation.c` now opens `save:0` on openfpgaOS. This maps to the same
nonvolatile slot 10 as the existing Pocket `sm64.sav`, preserving that data,
and to MiSTer's `slot_0.sav` backing file. The former basename existed only in
Pocket's slot configuration, preventing SM64 EEPROM writes on MiSTer. Desktop
and web save paths are unchanged. Both normal and profiling builds were rebuilt
after this correction; the local Pocket SD tree contains the normal build.

The SS1 created a fresh 512-byte EEPROM image with all eight save-copy signatures
and both menu-copy signatures valid. A restart test then supplied one valid
nondefault menu copy and a corrupt backup: the game retained the sound setting
and repaired the backup, proving both read and write persistence through the
portable alias. Another restart verified restoration of the original defaults.
Only the private test save image was used. The normal optimized game is left
running and available as `SM64 - GPU test` in the SS1's openfpgaOS game folder.
