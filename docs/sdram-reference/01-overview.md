# Analogue Pocket SDRAM Memory Architecture

## Overview

The Analogue Pocket uses a 64MB SDR SDRAM (AS4C32M16) with a 16-bit data bus. This document covers the memory architecture, clock domains, and integration patterns for custom cores.

## SDRAM Specifications

| Parameter | Value |
|-----------|-------|
| **Part Number** | AS4C32M16 |
| **Capacity** | 64MB (512Mbit) |
| **Organization** | 32M x 16-bit |
| **Banks** | 4 |
| **Rows** | 8192 per bank |
| **Columns** | 1024 per row |
| **Data Bus** | 16-bit |
| **Address Bus** | 13-bit (A0-A12) |
| **Bank Address** | 2-bit (BA0-BA1) |

## Memory Map

```
┌─────────────────────────────────────────────────────────────┐
│                    ADDRESS SPACE                             │
├─────────────────┬───────────────────────────────────────────┤
│ 0x00000000      │ Internal BRAM (64KB)                      │
│ 0x0000FFFF      │                                           │
├─────────────────┼───────────────────────────────────────────┤
│ 0x10000000      │ SDRAM Start                               │
│                 │ - APF Data Slots (loaded by bridge)       │
│ 0x12100000      │ - Heap region (CPU malloc)                │
│ 0x14000000      │ SDRAM End (64MB)                          │
├─────────────────┼───────────────────────────────────────────┤
│ 0x20000000      │ Terminal VRAM (8KB)                       │
├─────────────────┼───────────────────────────────────────────┤
│ 0x40000000      │ System Registers                          │
│                 │ - 0x00: SYS_STATUS                        │
│                 │ - 0x04: SYS_CYCLE_LO                      │
│                 │ - 0x08: SYS_CYCLE_HI                      │
└─────────────────┴───────────────────────────────────────────┘
```

## Clock Domains

The Analogue Pocket has multiple clock domains that must be carefully synchronized:

```
┌─────────────────────────────────────────────────────────────┐
│                    CLOCK DOMAINS                             │
├──────────────────┬──────────────────────────────────────────┤
│ clk_74a          │ 74.25 MHz - APF Bridge, Host Interface   │
│ (Reference)      │ Primary input clock from Pocket          │
├──────────────────┼──────────────────────────────────────────┤
│ clk_cpu          │ ~57 MHz - CPU, System Logic              │
│ (PLL derived)    │ PicoRV32 execution clock                 │
├──────────────────┼──────────────────────────────────────────┤
│ clk_ram          │ 133 MHz - SDRAM Controller               │
│ (PLL derived)    │ io_sdram.v state machine                 │
├──────────────────┼──────────────────────────────────────────┤
│ clk_ram_chip     │ 133 MHz -45° - SDRAM Chip Clock          │
│ (Phase shifted)  │ DQS alignment for data capture           │
└──────────────────┴──────────────────────────────────────────┘
```

### Clock Domain Crossing Paths

```
                    ┌─────────────┐
    CPU Domain      │   clk_cpu   │
    (~57 MHz)       │  PicoRV32   │
                    └──────┬──────┘
                           │ sdram_rd/wr
                           │ sdram_addr
                           │ sdram_wdata
                           ▼
                    ┌─────────────┐
    Bridge Domain   │  clk_74a    │  ◄── synch_3 CDC
    (74.25 MHz)     │  core_top   │
                    └──────┬──────┘
                           │ ram1_word_rd/wr
                           │ ram1_word_addr
                           │ ram1_word_data
                           ▼
                    ┌─────────────┐
    SDRAM Domain    │  clk_ram    │  ◄── synch_3 CDC (inside io_sdram.v)
    (133 MHz)       │  io_sdram   │
                    └──────┬──────┘
                           │
                           ▼
                    ┌─────────────┐
                    │   SDRAM     │
                    │  AS4C32M16  │
                    └─────────────┘
```

## Two Approaches: Direct vs LiteDRAM

### Approach 1: Direct io_sdram.v (AnaloguePicoRV32)

- Custom FSM-based SDRAM controller
- Simple word interface (32-bit)
- synch_3 synchronizers for CDC
- Busy/ready handshaking
- ~90ns per word access

**Pros:** Simple, transparent, low resource usage
**Cons:** Manual CDC management, timing-sensitive

### Approach 2: LiteDRAM (openfpga-litex)

- LiteDRAM framework with DFI interface
- Wishbone bus integration
- dcfifo (dual-clock FIFO) for CDC
- Automatic flow control
- Half-rate DDR PHY (114 MHz × 2)

**Pros:** Robust CDC, sophisticated controller, burst support
**Cons:** Complex, higher resource usage

## Key Files

| File | Purpose |
|------|---------|
| `src/fpga/core/io_sdram.v` | SDRAM controller (FSM, timing) |
| `src/fpga/core/core_top.v` | Top-level integration, CDC |
| `src/fpga/core/cpu_system.v` | CPU memory interface |
| `src/fpga/apf/common.v` | synch_3 synchronizer module |

## Quick Reference: SDRAM Signals

```verilog
// SDRAM Physical Interface
output wire        dram_clk,     // SDRAM clock
output wire        dram_cke,     // Clock enable
output wire        dram_ras_n,   // Row address strobe
output wire        dram_cas_n,   // Column address strobe
output wire        dram_we_n,    // Write enable
output wire [1:0]  dram_ba,      // Bank address
output wire [12:0] dram_a,       // Address bus
inout  wire [15:0] dram_dq,      // Data bus (bidirectional)
output wire [1:0]  dram_dqm      // Data mask
```

## Next Steps

1. [02-io-sdram-controller.md](02-io-sdram-controller.md) - SDRAM controller details
2. [03-clock-domain-crossing.md](03-clock-domain-crossing.md) - CDC patterns
3. [04-cpu-integration.md](04-cpu-integration.md) - CPU interface
