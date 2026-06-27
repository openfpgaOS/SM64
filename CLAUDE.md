# Project Instructions

Super Mario 64 is now an **openfpgaOS SDK custom core** (a self-contained fork
of the openfpgaSDK). It builds a single `app.elf` that runs on the shared
openfpgaOS **os30** bitstream (`runtime/pocket/os30.rbf_r`) — there is no longer
a per-core FPGA project. The game code lives under `src/sm64/`:

- `src/sm64/sm64/` — the SM64 PC-port engine (assets compiled in)
- `src/sm64/pocket/` — openfpgaOS platform backends (`of_video`/`of_input`/
  `of_audio` via `wm_pocket.c` / `controller_pocket.c` / `audio_pocket.c`,
  entry in `main_of.c`, fixed-point matrix math in `fx32_mtx.c`)
- `src/sm64/sm64/src/pc/gfx/gfx_gpu.c` — the Full-GPU renderer (os30 hardware
  vertex-triangle path via `src/sdk/include/of_gpu.h`)
- `src/sdk/`, `runtime/`, `scripts/`, `dist/sdk/` — vendored SDK (don't edit)
- `dist/sm64/` — this core's config (Cores/Platforms/Assets JSON)

## Build & Testing Workflow

After making changes to the firmware or renderer:

1. **Build the core** (RISC-V `app.elf` + SD-card tree under `build/pocket/sm64/`):
   ```bash
   cd src/sm64 && make
   ```

2. **Deploy to the Analogue Pocket** (auto-detects the SD card):
   ```bash
   cd src/sm64 && make copy
   ```
   Then boot the Pocket and select the SM64 core.

3. **Package a distributable ZIP:** `cd src/sm64 && make package`

4. **Desktop smoke build (optional, SDL2):** `cd src/sm64 && make test` → `./app_pc`

The build defines `TARGET_OPENFPGA`. The renderer is palettized (8-bit RGB332
framebuffer + 64-row palookup colormap, CI8 textures, per-vertex shade-row
lighting) — an intentional trade for running on the os30 hardware GPU.
