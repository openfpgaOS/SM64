# PocketSM64

Super Mario 64 running natively on the [Analogue Pocket](https://www.analogue.co/pocket) via a VexRiscv RISC-V soft CPU on the Cyclone V FPGA. No emulation — the sm64-port PC decompilation runs as bare-metal firmware on a hardware CPU synthesized in the FPGA fabric.

## Installation

1. Copy the contents of the `release/` directory to your Analogue Pocket SD card root
2. Place a US SM64 ROM (`baserom.us.z64`) in the build directory for asset extraction
3. See [Installation Layout](#installation-layout) below for the full SD card directory structure.

### Controls

| Button | Action |
|--------|--------|
| D-pad | Move |
| Left stick | Move (analog) |
| A (right face) | Jump |
| B (bottom face) | Attack/Dive |
| X (top face) | Punch |
| L1 | Crouch/Ground pound |
| R1 | Camera mode |
| Start | Pause |

## Features

- **SM64 PC port** — Software-rendered Super Mario 64 at 320x240
- **VexRiscv RISC-V CPU** — rv32imaf (integer, multiply/divide, atomics, single-precision FPU) at 110 MHz
- **Hardware span rasterizer** — FPGA accelerator for span drawing and z-buffer operations
- **48 kHz stereo audio** — I2S output with Bresenham upsampling
- **Dock support** — HDMI output when docked

## Architecture

```
+-----------------------------------------------------------------------+
|                       Analogue Pocket FPGA                            |
|                     (Cyclone V 5CEBA4F23C8)                           |
+-----------------------------------------------------------------------+
|                                                                       |
|  +-----------------------+    +-----------------------------------+   |
|  |    VexRiscv CPU       |    |        Memory Subsystem            |   |
|  |  rv32imaf @ 110 MHz   |    |                                   |   |
|  |                       |    |  +--------+  +--------+  +------+ |   |
|  |  I$ 32KB  (2-way)     |    |  | BRAM   |  | SDRAM  |  | PSRAM| |   |
|  |  D$ 128KB (2-way)     |    |  | 64KB   |  | 64MB   |  | 16MB | |   |
|  +-----------+-----------+    |  +--------+  +--------+  +------+ |   |
|              |                |              +--------+            |   |
|              |                |              | SRAM   |            |   |
|              |                |              | 256KB  |            |   |
|              |                +---+----------+--------+-----------++   |
|              |                    |                                    |
|  +-----------+--------------------+----------------------------+      |
|  |                    VexRiscv AXI Bus                         |      |
|  +----+--------+----------+---------+---------+--------+------++      |
|       |        |          |         |         |        |       |      |
|  +----+--+ +---+----+ +--+---+ +---+---+ +---+--+ +---+--+ +-+----+ |
|  | Video | | Audio  | | Span | | Link  | | Sys  | | SRAM | | Cmap | |
|  |Scanout| | Output | | Rast | | MMIO  | | Regs | | Fill | | BRAM | |
|  +-------+ +--------+ +------+ +-------+ +------+ +------+ +------+ |
|                                                                       |
+-----------------------------------------------------------------------+
```

## Memory Map

| Address Range             | Size   | Description                                          |
|---------------------------|--------|------------------------------------------------------|
| `0x00000000 - 0x0000FFFF` | 64 KB  | BRAM — bootloader, hot code (.fasttext), sin tables   |
| `0x10000000 - 0x10012BFF` | 75 KB  | Framebuffer 0 (320x240, 8-bit indexed)               |
| `0x10100000 - 0x10112BFF` | 75 KB  | Framebuffer 1 (double buffer)                        |
| `0x10200000`              | ~2 MB  | sm64.bin load address (LMA, copied to PSRAM)         |
| `0x11000000`              | ~18 MB | Asset data (memory-mapped)                           |
| `0x12400000`              | ~28 MB | BSS + heap                                           |
| `0x20000000`              | 1.2 KB | Terminal VRAM (40x30 characters)                     |
| `0x30000000 - 0x30FFFFFF` | 16 MB  | PSRAM/CRAM0 — App code + rodata + data (VMA)         |
| `0x38000000 - 0x3803FFFF` | 256 KB | SRAM — Z-buffer (153 KB used)                        |
| `0x40000000`              | 256 B  | System registers                                     |
| `0x48000000`              | 256 B  | Span rasterizer registers                            |
| `0x4C000000`              | 8 B    | Audio FIFO (write samples / read status)             |
| `0x4D000000`              | 256 B  | Link cable MMIO registers                            |
| `0x50000000 - 0x53FFFFFF` | 64 MB  | SDRAM uncached alias (bypasses D-cache)              |
| `0x54000000`              | 16 KB  | Colormap BRAM                                        |
| `0x58000000`              | 8 KB   | Alias Transform MAC (registers + normal table)       |
| `0x5C000000`              | 16 B   | SRAM fill engine registers                           |

## System Registers (0x40000000)

| Offset | Register       | Description                                    |
|--------|----------------|------------------------------------------------|
| 0x00   | SYS_STATUS     | [0] sdram_ready, [1] allcomplete               |
| 0x04   | CYCLE_LO       | Cycle counter (low 32 bits)                    |
| 0x08   | CYCLE_HI       | Cycle counter (high 32 bits)                   |
| 0x0C   | DISPLAY_MODE   | 0 = terminal overlay, 1 = framebuffer          |
| 0x10   | FB_DISPLAY     | Display framebuffer address (25-bit word addr) |
| 0x14   | FB_DRAW        | Draw framebuffer address (25-bit word addr)    |
| 0x18   | FB_SWAP        | Write 1 to swap on next vsync                 |
| 0x40   | PAL_INDEX      | Palette write index (auto-increment)           |
| 0x44   | PAL_DATA       | Palette entry (RGB888, triggers write)         |

## Hardware Accelerators

### Span Rasterizer

The span rasterizer is an FPGA state machine that offloads inner rendering loops. It handles:

- **Textured span drawing** — Fetches texels from SDRAM and writes 8-bit pixels to the framebuffer
- **Z-span writing** — Writes z-buffer values to SRAM
- **Surface block rendering** — Processes surface vblocks with hardware bilinear light interpolation
- **Colormap lookup** — 16 KB BRAM stores the colormap for light-level application
- **Turbulence** — 128-entry sine LUT for water/lava warping

### SRAM Fill Engine

Autonomously clears the z-buffer in SRAM while the CPU performs frame setup work.

### SRAM Arbitration

Three-way priority mux for SRAM access: CPU > span rasterizer > fill engine.

## Video Pipeline

- **Resolution:** 320x240 @ 60 Hz (12.288 MHz pixel clock)
- **Color depth:** 8-bit indexed with 256-entry RGB888 hardware palette
- **Scanout:** Burst reads from SDRAM (80 bursts x BL=2 = 320 pixels/line)
- **Double buffered:** CPU draws to back buffer, `FB_SWAP` swaps on vsync
- **Clock domain crossing:** Dual-clock FIFO between pixel clock (12.288 MHz) and SDRAM clock (110 MHz)

## Audio Pipeline

- **Sample rate:** 48 kHz stereo, 16-bit signed
- **I2S output:** MCLK 12.288 MHz, SCLK 3.072 MHz, LRCK 48 kHz
- **FIFO:** 4096-entry dual-clock FIFO (CPU clock to audio clock)
- **Interface:** CPU writes 32-bit stereo samples `{L16, R16}` to MMIO 0x4C000000

## Boot Flow

1. FPGA configures, BRAM bootloader runs from address 0x00000000
2. APF bridge loads `sm64.bin` into SDRAM at 0x10200000
3. APF bridge loads assets into SDRAM at 0x11000000
4. Bootloader copies `sm64.bin` from SDRAM to PSRAM (0x30000000)
5. `fence` + `fence.i` (flush D-cache, invalidate I-cache)
6. Jump to `sm64_main` in PSRAM

## Building

### Prerequisites

- **RISC-V toolchain:** `riscv64-elf-gcc` with rv32imaf support
- **Intel Quartus Prime:** 25.1 or later (Lite edition sufficient)
- **Analogue Pocket:** Firmware 2.2 or later

```bash
# Arch Linux
sudo pacman -S riscv64-elf-gcc riscv64-elf-newlib

# The firmware uses -march=rv32imaf -mabi=ilp32f
```

### Build Firmware

```bash
cd src/firmware
make                  # Builds sm64.bin + firmware.mif
make install          # Copies MIF to FPGA directory
```

### Build FPGA

```bash
cd src/fpga
make                  # Full Quartus synthesis (~15 min)
make mif              # Update MIF only, no resynthesis (~1 min)
make program          # Program via JTAG (USB Blaster)
```

### Package Release

```bash
make                  # From project root — packages release/ directory
```

## Installation Layout

```
SD Card Root/
+-- Assets/
|   +-- pocketsm64/
|       +-- common/
|           +-- sm64.bin
|           +-- sm64_assets.bin
+-- Cores/
|   +-- ThinkElastic.PocketSM64/
|       +-- bitstream.rbf_r
|       +-- core.json
|       +-- (other .json files)
+-- Platforms/
    +-- _images/
    |   +-- pocketsm64.bin
    +-- pocketsm64.json
```

## Project Structure

```
.
+-- src/
|   +-- firmware/                  # SM64 firmware (C, bare-metal)
|   |   +-- main.c                 # Bootloader
|   |   +-- sm64/                  # SM64 PC port source
|   |   |   +-- (sm64-port files)
|   |   +-- pocket/                # Platform adaptation layer
|   |   |   +-- wm_pocket.c        # Window manager
|   |   |   +-- gfx_pocket.c       # Graphics backend
|   |   |   +-- audio_pocket.c     # Audio backend
|   |   |   +-- controller_pocket.c # Input backend
|   |   +-- libc/                  # Minimal C library
|   |   +-- linker.ld              # Linker script (BRAM/PSRAM/SDRAM layout)
|   |   +-- Makefile
|   |
|   +-- fpga/                      # FPGA design (Verilog)
|   |   +-- core/
|   |   |   +-- core_top.v         # Top-level: CPU + bus + peripherals
|   |   |   +-- io_sdram.v         # SDRAM controller
|   |   |   +-- span_rasterizer.v  # Hardware span/texture rasterizer
|   |   |   +-- video_scanout_indexed.v  # 8-bit indexed video scanout
|   |   |   +-- audio_output.v     # I2S audio output with FIFO
|   |   |   +-- sram_fill.v        # Z-buffer clear engine
|   |   |   +-- text_terminal.v    # Debug text overlay
|   |   +-- vexriscv/
|   |   |   +-- VexRiscv_Full.v    # Generated RISC-V CPU core
|   |   |   +-- generate.sh        # SpinalHDL generation script
|   |   +-- apf/                   # Analogue Pocket framework (bridge, I/O)
|   |   +-- Makefile
|   |
|   +-- firmware_test/             # Hardware test firmware (SDRAM/PSRAM/CPU tests)
|
+-- dist/                          # Platform images and icons
+-- release/                       # Packaged release for SD card
+-- tools/
|   +-- capture_ocr.sh             # HDMI capture + OCR testing tool
+-- Makefile                       # Top-level build/package
+-- deploy.sh                      # Quick deploy to SD card
+-- *.json                         # APF configuration files
```

## Important Notes

- **JTAG programming loses SDRAM data.** After JTAG programming, the Pocket must reload `sm64.bin` and assets from the SD card. Always deploy both firmware and bitstream to the SD card for testing.
- **Firmware and FPGA must match.** The BRAM initialization (MIF) is compiled into the bitstream. If `sm64.bin` on the SD card doesn't match the MIF in the FPGA, `.fasttext` function calls will jump to wrong addresses and crash.
- **RVC is disabled** for timing closure at 110 MHz. The firmware links against a custom `libgcc_norvc.a`.

## License

- **sm64-port:** See sm64-port license (decompilation project)
- **VexRiscv:** MIT (SpinalHDL)
- **PocketSM64 (FPGA/firmware):** MIT

## Acknowledgments

- [sm64-port](https://github.com/fgsfdsfgs/sm64-port) — SM64 PC port with software renderer
- [SpinalHDL/VexRiscv](https://github.com/SpinalHDL/VexRiscv) — RISC-V CPU core
- [Analogue](https://www.analogue.co/developer) — Pocket openFPGA development framework
