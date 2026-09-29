# SM64 rendering corrections — 2026-09-14

The MiSTer full-GPU build exposed incorrect colors, texture discontinuities
between triangles, and corrupted character textures. This review found separate
problems in SM64's renderer and the shared GPU. The fixes retain the existing
GPU command ABI and leave game logic, animation, audio and save formats intact.

A subsequent user report exposed further bugs in MiSTer's Direct FB channel
order and the shared GPU's pixel-center coverage. Those two fixes required only a new core. The later body-triangle, HUD and
performance work requires an updated game executable as well as the new core. See the sibling openfpgaOS repository's
`docs/MISTER_COLOR_EDGE_FIXES_20260914.md` for the follow-up evidence.

The later pass is documented in the sibling openfpgaOS repository
`docs/SM64_BODY_PERFORMANCE_20260914.md`. It fixes small-triangle depth setup and
uninitialized HUD rectangle attributes, adds capability-gated CPU command-ring
submission, starts GPU frame clears earlier, and replaces a clipped-vertex
64-bit division with an exact RV32 calculation. SM64 also uses measured dual-issue
compiler scheduling without changing desktop compiler flags. The hardware
measurements below describe the earlier rendering-only build.

## Corrections

- **Fractional attribute origins (RTL):** gradients used Q12.4 vertices, but
  the origin calculation rounded the anchor to integer pixels before multiplying.
  Keeping the fraction through multiplication aligns UV, color and depth planes.
- **Distant texture precision (RTL):** cached/clip/RGB transform commands now
  normalize the three perspective attributes together before deriving gradients.
  Scaling cancels in the texture-coordinate ratio. The independent depth values
  stay unchanged; scalar/palettized commands retain their original depth scale.
  A shared enable shifts all attributes one bit per cycle using fixed wiring;
  it avoids a magnitude scan, absolute-value adders and barrel shifters.
- **Shared-edge projection (SM64):** cached and fallback triangles use the same
  clip-coordinate quantization and reciprocal on both axes. Scaling the Y viewport
  constants by 16 restores Q12.4 Y without a new command. Original clip X/Y/W are
  saved for every fallback, including material changes and clipped triangles.
- **Color inputs:** RGB and alpha use their own combiner input maps. An alpha-only
  input no longer replaces the shade color. Opaque black uses RGB565 0x0001 instead
  of the visibly blue 0x0008; zero remains the transparent key.
- **Mario's decals:** binary texel-alpha interpolation draws the shaded base
  through a complementary mask and the original opaque texels through the ordinary
  mask. Each pixel blends once, including during fades. This preserves the shaded
  surface where texture alpha is zero, instead of discarding that surface.
- **Texture ownership:** buffers own complete 64-byte cache lines. Fresh buffers
  drain CPU writeback before uncached stores. A third use within a frame waits for
  queued GPU reads before overwriting/freeing the buffer. Uploads invalidate the
  GPU texture cache, which otherwise could retain earlier contents at reused addresses.
- **Decal depth bias:** Y deltas are converted from Q12.4 to pixels before computing
  the slope. The existing bias constants are unchanged.
- **Capability handling:** the HILITE combiner is enabled only when advertised.

## Regression evidence

- The independent fractional-origin test failed on all 25 checked pixels before
  the RTL correction and passes after it.
- The distant-texture test checks an analytic UV plane at 253 pixel centers.
  Disabling normalization produces 252 mismatches; enabling it produces none.
- MiSTer acceptance, stalled-memory acceptance, Pocket os30 acceptance and native
  software/sanitizer checks are recorded in the accompanying measurements.
- Software regressions exhaust all 65,536 RGBA5551 inputs and check the transparent
  key, complementary masks, two-pass decal commands, independent RGB/alpha inputs,
  cache invalidation, safe third-upload reuse, and fallback subpixel positions.

## Hardware validation

SS1 at 100 MHz completed 4,800 simulation ticks over five demos with audio enabled.
Every recorded game-state and camera vector matches the previous build. Physical
presentation averages were 16.82, 15.56, 16.87, 16.86 and 14.03 FPS in demo order,
2.7–4.9% below the previous optimized build. Correct projection, texture ownership
and decal composition add work; this is a rendering correction, not a speedup.
A CPU projection-cache experiment was removed after it slowed all five demos.

Private framebuffer captures at simulation frames 180 and 600 cover the title
head and Bowser demo. With projection held constant, the corrected cached path
matches 99.51% of gameplay pixels against the cache-disabled reference; the bottom
55-row floor region matches exactly. The final hardware simplification preserves
both corrected captures pixel-for-pixel. Captures and diagnostic builds remain
private because they contain assets from the user's ROM.

The selected MiSTer fit uses 29,921 ALMs, 419 RAM blocks and 69 DSPs. Worst setup
slack is −0.322 ns at 100 MHz: setup timing is not closed. Hold (+0.073 ns) and
all checked SDRAM read/write timing corners pass. The shared RTL passes 322
MiSTer checks, 323 stressed-memory checks and 169 Pocket os30 checks; software
regressions pass natively and under ASan/UBSan. Pocket has simulation coverage,
but no new Pocket bitstream was fitted or tested on hardware in this pass.

The SS1's **SM64 - GPU test** launcher has the corrected normal game and core.
The core and boot-image hashes were verified remotely, slot 0 is unchanged, and
the previous private installation was backed up. The selected core also passes
the Doom E1M1 reference rotation: 60.015 FPS average, 59 FPS minimum one-second
window and zero recorded MIDI envelope overruns.

Detailed asset-free results are in the sibling openfpgaOS repository:
`docs/SM64_RENDERING_20260914.md` and
`docs/measurements/sm64-rendering-20260914/`.

The pre-existing HILITE signed-coefficient range and nearest-neighbor texture
filtering remain renderer limitations; these changes do not claim pixel-identical
N64 rendering.

## Later performance pass

The current **SM64 - GPU test** installation on the SS1 includes the later
small-triangle/HUD corrections and the selected command-submission, memory-write
and CPU improvements. Five-scene geometric mean FPS is 25.35% above the corrected
DMA reference; the requested 30% has not been reached. All 4,800 game/camera
states and eight fixed animation images match the corrected reference. Save
slot 0 is preserved. The selected 100 MHz core uses 30,653 ALMs with −0.398 ns
setup, +0.092 ns hold and passing SDRAM DQ checks. See the sibling core report
`docs/SM64_BODY_PERFORMANCE_20260914.md` for the selected artifacts and rejected
experiments. The earlier measurements above remain the rendering-only history.

## Menu and shadow follow-up

Truecolor fills preserve the requested color, viewport/scissor Y coordinates are
converted to framebuffer coordinates, and translucent alpha zero remains fully
transparent. Each triangle selects its own alpha when a batch spans a material
alpha change. These correct the dark act-selection background, misplaced dialog
clipping and Peach-letter fade flashes.

The decal depth offset is reduced from 1.2% plus four pixels of slope to 0.1% plus
half a pixel, preventing reproduced ground-shadow overdraw on Mario's overalls
and Bowser's feet. Eight SS1 poses change 164 pixels, with 162 closer to the
software reference. Native and sanitizer regressions cover the new behavior.
Controlled SS1 captures confirm that Peach's letter changes zero menu pixels at
both zero-alpha endpoints, excluding the rotating star. The normal game is
installed in **SM64 - GPU test**, with save slot 0 preserved and the core unchanged.
Full measurements and limitations are in the core repository's
`docs/SM64_MENU_SHADOW_FIXES_20260914.md`.

## Inherited material order

Individual scene-graph display lists retain their original order in every layer.
The previous depth sort separated body parts that deliberately share lighting:
Mario's overalls and lower legs could inherit red, white or brown from other
parts. The sort and its per-node key are removed; the model data is unchanged.
An asset-free regression checks inherited materials across 120 relative depth
permutations, three layers and both depth-buffer modes. It fails on the previous
code and passes on the corrected code, including under ASan/UBSan.

The SS1 trace improves from 829 incorrectly lit draws out of 1,158 to zero.
The corrected normal game is installed in **SM64 - GPU test**, preserving save
slot 0 and the C11 core. One matched Bowser-demo performance pair measures
21.138 FPS before and 20.799 after; both have a 16 FPS minimum one-second window.

See the sibling core report `docs/SM64_MATERIAL_ORDER_FIX_20260914.md` for SS1
measurements and installation details.
