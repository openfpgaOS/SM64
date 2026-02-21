# SDRAM Timing Analysis

## Overview

Understanding SDRAM timing is critical for reliable operation. This document explains timing parameters, how they map to clock cycles, and how to analyze latency in the Analogue Pocket design.

## SDRAM Timing Parameters

### AS4C32M16 Specifications

The Analogue Pocket uses AS4C32M16 SDRAM. Key timing parameters at -6 speed grade:

| Parameter | Symbol | Min | Max | Description |
|-----------|--------|-----|-----|-------------|
| Clock Period | tCK | 6ns | - | Minimum clock period |
| Row Precharge | tRP | 18ns | - | Time to precharge a row |
| RAS-to-CAS Delay | tRCD | 18ns | - | Row activate to column access |
| CAS Latency | CL | 2/3 | - | Column address to data out |
| Write Recovery | tWR | 15ns | - | Last write to precharge |
| Row Active Time | tRAS | 48ns | 100µs | Minimum row active period |
| Row Cycle Time | tRC | 66ns | - | Row cycle (tRAS + tRP) |
| Refresh Cycle | tRFC | 80ns | - | Auto-refresh cycle time |
| Refresh Interval | tREFI | 7.8µs | - | Maximum between refreshes |

### Timing at 133 MHz

At 133 MHz (7.5ns clock period), timing parameters map to cycles:

```
tRP   = 18ns  → ceil(18/7.5)  = 3 cycles
tRCD  = 18ns  → ceil(18/7.5)  = 3 cycles
tWR   = 15ns  → ceil(15/7.5)  = 2 cycles
tRAS  = 48ns  → ceil(48/7.5)  = 7 cycles
tRC   = 66ns  → ceil(66/7.5)  = 9 cycles
tRFC  = 80ns  → ceil(80/7.5)  = 11 cycles
CL    = 3 cycles (selected in Mode Register)
```

### io_sdram.v Timing Constants

```verilog
// From io_sdram.v (lines 69-79)
localparam TIMING_POWERUP = 26666;     // 200µs at 133MHz
localparam TIMING_REFRESH = 1000;      // Refresh counter (~7.5µs)
localparam TIMING_ACT_RW = 3;          // tRCD: 3 cycles
localparam TIMING_PRECHARGE = 3;       // tRP: 3 cycles
localparam TIMING_WRITE = 2;           // tWR: 2 cycles
localparam CAS_LATENCY = 3;            // CL=3
```

## Operation Timing Diagrams

### Word Read Timing

```
Cycle:     0    1    2    3    4    5    6    7    8    9   10   11
           │    │    │    │    │    │    │    │    │    │    │    │
Clock: ____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____

State:     │IDLE│    ST_READ_0     │    ST_READ_1-4    │  5  │  6  │ 7  │
           │    │ ACT │ --- │ --- │ RD  │ RD  │ --- │ --- │DATA│DATA│PRCH│
Command:   │NOP │ ACT │ NOP │ NOP │READ │READ │ NOP │ NOP │ NOP│ NOP│PRCH│
           │    │     │     │     │     │     │     │     │    │    │    │
           │    │     │<-tRCD(3)->│     │     │<-CL(3)->│    │    │    │
                                                         ▲    ▲
                                                       Hi16  Lo16

Total Read Latency: ~12 cycles = 90ns
```

### Word Write Timing

```
Cycle:     0    1    2    3    4    5    6    7    8    9   10
           │    │    │    │    │    │    │    │    │    │    │
Clock: ____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____

State:     │IDLE│   ST_WRITE_0-1   │  2  │  3  │  4  │  5  │IDLE│
           │    │ ACT │ --- │ --- │ WR  │ WR  │ --- │PRCH │    │
Command:   │NOP │ ACT │ NOP │ NOP │WRIT │WRIT │ NOP │PRCH │NOP │
Data:      │    │     │     │     │Hi16 │Lo16 │     │     │    │
           │    │     │     │     │     │     │     │     │    │
           │    │     │<-tRCD(3)->│     │<tWR>│<tRP>│     │

Total Write Latency: ~10 cycles = 75ns
```

### Refresh Timing

```
Cycle:     0    1    2    3    4    5    6    7    8    9   10   11
           │    │    │    │    │    │    │    │    │    │    │    │
Clock: ____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____

State:     │IDLE│ ST_REFRESH_0-7                              │IDLE│
Command:   │NOP │PALL │ NOP│ REF │ NOP│ NOP│ NOP│ NOP│ NOP│ NOP│NOP │
           │    │     │    │     │    │    │    │    │    │    │    │
           │    │<tRP>│    │<-------- tRFC (11 cycles) ------>│

Total Refresh Latency: ~12 cycles = 90ns
```

## Total Access Latency Analysis

### CPU to SDRAM Round-Trip

Complete latency from CPU request to data available:

```
┌───────────────────────────────────────────────────────────────────┐
│                    LATENCY BREAKDOWN                               │
├───────────────────────────────────────────────────────────────────┤
│                                                                    │
│  CPU Request → Bridge Domain                                       │
│  ├── synch_3 latency: 3 × 13.5ns = 40.5ns                         │
│                                                                    │
│  Bridge Domain → SDRAM Controller                                  │
│  ├── synch_3 latency: 3 × 7.5ns = 22.5ns                          │
│                                                                    │
│  SDRAM Operation (Read)                                            │
│  ├── ACT + tRCD: 4 cycles = 30ns                                  │
│  ├── READ + CAS: 4 cycles = 30ns                                  │
│  ├── Data capture: 2 cycles = 15ns                                │
│  ├── PRECHARGE: 3 cycles = 22.5ns                                 │
│  └── Total SDRAM: ~12 cycles = 90ns                               │
│                                                                    │
│  SDRAM Controller → Bridge Domain                                  │
│  ├── Data valid propagation: ~1 cycle = 7.5ns                     │
│                                                                    │
│  Bridge Domain → CPU (busy signal)                                 │
│  ├── synch_3 latency: 3 × 17.5ns = 52.5ns                         │
│                                                                    │
│  TOTAL ROUND-TRIP: ~213ns = ~12 CPU cycles @ 57MHz                │
│                                                                    │
└───────────────────────────────────────────────────────────────────┘
```

### Latency by Clock Domain

| Domain | Clock | Period | Sync Latency |
|--------|-------|--------|--------------|
| CPU | ~57 MHz | 17.5ns | - |
| Bridge (clk_74a) | 74.25 MHz | 13.5ns | 40.5ns from CPU |
| SDRAM (clk_ram) | 133 MHz | 7.5ns | 22.5ns from Bridge |

### Effective Bandwidth

```
Single Word Access:
- 4 bytes per 213ns = 18.8 MB/s

Burst Access (8 words):
- 32 bytes per ~300ns = 106 MB/s
- First word: 90ns
- Each additional: ~30ns (no row activation)

Theoretical Maximum (133 MHz, 16-bit):
- 133M × 2 bytes = 266 MB/s
- With bank interleaving: ~500 MB/s
```

## Refresh Impact

### Refresh Requirements

```
SDRAM Refresh Requirements:
- 8192 rows must be refreshed every 64ms
- 64ms / 8192 = 7.81µs per row
- At 133 MHz: 7.81µs / 7.5ns = 1041 cycles between refreshes
```

### Refresh Counter in io_sdram.v

```verilog
// Refresh counter wraps every ~7.5µs (line 552-560)
refresh_count <= refresh_count + 1'b1;
if(&refresh_count) begin  // All 1s = wrap
    issue_autorefresh <= 1;
end

// 10-bit counter: 2^10 = 1024 cycles
// 1024 × 7.5ns = 7.68µs (within 7.81µs requirement)
```

### Worst-Case Latency

```
Worst case: Request arrives just as refresh starts

Refresh overhead: 12 cycles = 90ns
Added latency:    90ns + normal access = 180ns

With queued request waiting:
- Refresh: 90ns
- Read: 90ns
- Total: 180ns = ~10 CPU cycles
```

## Setup and Hold Analysis

### Data Capture Window

```
SDRAM data output (read):
- Data valid: tAC (max 6ns after CL cycles)
- Data hold: tOH (min 2.5ns)

At 133 MHz (7.5ns period):
- Sample window: tOH to tCK = 2.5ns to 7.5ns
- Window size: 5ns

clk_ram_chip phase shift: -45° = -0.94ns
- Shifts sample point earlier for better margin
```

### Write Data Setup

```
SDRAM data input (write):
- Setup time: tDS = 1.5ns (min before clock)
- Hold time: tDH = 1.0ns (min after clock)

Controller must:
- Assert data 1.5ns before rising edge
- Hold data 1.0ns after rising edge
```

## Mode Register Settings

```verilog
// Mode Register configuration (io_sdram.v)
// A[2:0]   = Burst Length (3'b011 = 8)
// A[3]     = Burst Type (0 = Sequential)
// A[6:4]   = CAS Latency (3'b011 = 3)
// A[8:7]   = Operating Mode (2'b00 = Standard)
// A[9]     = Write Burst Mode (0 = Programmed Length)

localparam MODE_REG = 13'b0_00_011_0_011;  // CL=3, BL=8
```

## Optimization Strategies

### 1. Reduce CDC Latency

```
Current: synch_3 = 3 cycles
Option:  synch_2 = 2 cycles (higher metastability risk)

Savings: 1 cycle per crossing × 2 crossings = ~27ns
```

### 2. Bank Interleaving

```
Open multiple banks simultaneously:
- While waiting for tRCD on bank 0, activate bank 1
- Interleave reads across banks
- Effective latency reduction: ~40%
```

### 3. Page Mode Access

```
Keep row open for sequential accesses:
- First access: ACT + tRCD + READ + CL = 10 cycles
- Subsequent (same row): READ + CL = 4 cycles
- Savings: 6 cycles (45ns) per access
```

### 4. Prefetch Buffer

```
CPU-side prefetch:
- Predict next address during current access
- Issue speculative read
- Hide SDRAM latency behind computation
```

## Timing Verification

### Simulation Checklist

1. **tRCD compliance**: Verify 3+ cycles between ACT and READ/WRITE
2. **tRP compliance**: Verify 3+ cycles between PRECHARGE and next ACT
3. **tWR compliance**: Verify 2+ cycles between last write and PRECHARGE
4. **tRAS compliance**: Verify row stays active 7+ cycles
5. **tRC compliance**: Verify 9+ cycles between consecutive ACT to same bank
6. **Refresh interval**: Verify refresh within 7.8µs

### SignalTap Debug Signals

```verilog
// Key signals to monitor
.probe(state),
.probe(cmd),
.probe(dc),              // Delay counter
.probe(word_rd_r),       // Synchronized read request
.probe(word_wr_r),       // Synchronized write request
.probe(word_busy),       // Busy flag
.probe(refresh_count)    // Refresh timing
```

## See Also

- [02-io-sdram-controller.md](02-io-sdram-controller.md) - FSM implementation
- [03-clock-domain-crossing.md](03-clock-domain-crossing.md) - CDC latency details
- [06-gotchas-pitfalls.md](06-gotchas-pitfalls.md) - Timing-related issues
