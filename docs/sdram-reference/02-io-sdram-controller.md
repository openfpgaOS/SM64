# io_sdram.v - SDRAM Controller Reference

## Overview

The `io_sdram.v` module is a custom FSM-based SDRAM controller from the Analogue reference design. It provides both burst and single-word interfaces for SDRAM access.

**File:** `src/fpga/core/io_sdram.v`

## Interfaces

### Word Interface (Single 32-bit Access)

```verilog
// Word interface signals
input   wire            word_rd,        // Read request (CDC from other domain)
input   wire            word_wr,        // Write request (CDC from other domain)
input   wire    [23:0]  word_addr,      // 24-bit word address (64MB addressable)
input   wire    [31:0]  word_data,      // 32-bit write data
output  reg     [31:0]  word_q,         // 32-bit read data
output  reg             word_busy       // Operation in progress
```

### Burst Interface (High-Bandwidth)

```verilog
// Burst read interface
input   wire            burst_rd,
input   wire    [24:0]  burst_addr,
input   wire    [10:0]  burst_len,
input   wire            burst_32bit,    // 1=32-bit mode, 0=16-bit mode
output  reg     [31:0]  burst_data,
output  reg             burst_data_valid,
output  reg             burst_data_done,

// Burst write interface
input   wire            burstwr,
input   wire    [24:0]  burstwr_addr,
output  wire            burstwr_ready,
input   wire            burstwr_strobe,
input   wire    [15:0]  burstwr_data,
output  reg             burstwr_done
```

## State Machine Architecture

The controller uses a hierarchical state machine with the following states:

```
┌─────────────────────────────────────────────────────────────┐
│                    STATE MACHINE                             │
├─────────────────────────────────────────────────────────────┤
│  ST_INIT_*     │ Power-up initialization sequence           │
│  ST_IDLE       │ Ready for new commands                     │
│  ST_REFRESH_*  │ Auto-refresh cycle                         │
│  ST_READ_*     │ Read operation (word or burst)             │
│  ST_WRITE_*    │ Write operation (word only)                │
│  ST_BURSTWR_*  │ Burst write operation                      │
└─────────────────────────────────────────────────────────────┘
```

### Initialization Sequence

```
ST_INIT_0 → ST_INIT_1 → ST_INIT_2 → ST_INIT_3 → ST_INIT_4 → ST_IDLE

1. Wait 200µs (power-up delay)
2. Issue PRECHARGE ALL command
3. Issue 2x AUTO REFRESH commands
4. Issue MODE REGISTER SET command
5. Enter IDLE state
```

### Word Read Sequence

```verilog
// ST_READ_0: Activate row
phy_ba <= addr[24:23];           // Bank select
phy_a <= addr[22:10];            // Row address (13 bits)
cmd <= CMD_ACT;
state <= ST_READ_1;

// ST_READ_1: Wait for tRCD
if(dc == TIMING_ACT_RW-1) begin
    state <= ST_READ_2;
end

// ST_READ_2: Issue READ command (first half)
phy_a <= addr[9:0];              // Column address (10 bits)
cmd <= CMD_READ;
enable_dq_read <= 1;
length <= length - 1;            // 2 reads for 32-bit word
addr <= addr + 1;
state <= ST_READ_5;

// ST_READ_5-9: Wait for CAS latency, capture data

// Data capture (delayed by CAS latency):
if(enable_dq_read_4) begin
    if(~enable_dq_read_toggle) begin
        word_q[31:16] <= phy_dq;  // High half first
    end else begin
        word_q[15:0] <= phy_dq;   // Low half second
    end
end

// ST_READ_6: Precharge
cmd <= CMD_PRECHG;
state <= ST_READ_7;

// ST_READ_7: Return to idle
state <= ST_IDLE;
```

### Word Write Sequence

```verilog
// ST_WRITE_0: Activate row
phy_ba <= addr[24:23];
phy_a <= addr[22:10];            // Row address
cmd <= CMD_ACT;
state <= ST_WRITE_1;

// ST_WRITE_1: Wait for tRCD
if(dc == TIMING_ACT_RW-1) begin
    phy_dq_oe <= 1;              // Enable output driver
    state <= ST_WRITE_2;
end

// ST_WRITE_2: Write high half
phy_a <= addr[9:0];              // Column address
cmd <= CMD_WRITE;
phy_dq_out <= word_data[31:16];  // High 16 bits first
addr <= addr + 1;
state <= ST_WRITE_3;

// ST_WRITE_3: Write low half
phy_a <= addr[9:0];
cmd <= CMD_WRITE;
phy_dq_out <= word_data[15:0];   // Low 16 bits second
addr <= addr + 1;
state <= ST_WRITE_4;

// ST_WRITE_4: Wait for tWR
if(dc == TIMING_WRITE-1+1) begin
    cmd <= CMD_PRECHG;
    state <= ST_WRITE_5;
end

// ST_WRITE_5: Return to idle
state <= ST_IDLE;
```

## Timing Parameters

```verilog
// At 133 MHz, 1 cycle = 7.5ns

// SDRAM Timing Parameters (lines 69-79)
localparam TIMING_POWERUP = 26666;     // 200µs power-up delay
localparam TIMING_REFRESH = 1000;      // Refresh cycle time
localparam TIMING_ACT_RW = 3;          // tRCD: RAS-to-CAS delay (3 cycles = 22.5ns)
localparam TIMING_PRECHARGE = 3;       // tRP: Precharge time (3 cycles = 22.5ns)
localparam TIMING_WRITE = 2;           // tWR: Write recovery (2 cycles = 15ns)
localparam CAS_LATENCY = 3;            // CAS latency (3 cycles)
```

### Timing Diagram

```
Clock:     ____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____/‾‾‾‾\____
           │  0  │  1  │  2  │  3  │  4  │  5  │  6  │  7  │  8  │  9  │ 10 │

READ:      │ ACT │ --- │ --- │ RD  │ --- │ --- │ --- │DATA │DATA │PRCH │IDLE│
                       tRCD(3)              CAS(3)     Hi   Lo

WRITE:     │ ACT │ --- │ --- │ WR  │ WR  │ --- │PRCH │ --- │ --- │IDLE │
                       tRCD(3)  Hi    Lo   tWR(2)     tRP(3)
```

## Address Mapping

The 24-bit word address maps to SDRAM as follows:

```
word_addr[23:0] = {bank[1:0], row[12:0], column[8:0]}

Physical SDRAM address:
- addr[24:23] → Bank Address (BA1:BA0)
- addr[22:10] → Row Address (A12:A0)
- addr[9:0]   → Column Address (A9:A0)

For 32-bit word access, column is shifted:
- addr <= word_addr << 1  // Each word = 2 x 16-bit locations
```

## Byte Ordering (16-bit to 32-bit)

**CRITICAL:** The byte ordering must match between read and write paths.

```verilog
// WRITE: High half to lower address, low half to higher address
ST_WRITE_2: phy_dq_out <= word_data[31:16];  // First write (addr N)
ST_WRITE_3: phy_dq_out <= word_data[15:0];   // Second write (addr N+1)

// READ: First read to high half, second read to low half
if(~enable_dq_read_toggle) begin
    word_q[31:16] <= phy_dq;   // First read → bits 31:16
end else begin
    word_q[15:0] <= phy_dq;    // Second read → bits 15:0
end
```

**Note:** This ordering matches the APF bridge burst writes. If you need little-endian byte ordering for CPU access, you may need to swap the halves.

## Clock Domain Crossing (Internal)

The io_sdram.v module includes synchronizers for signals coming from other clock domains:

```verilog
// Synchronize control signals to controller clock (133 MHz)
synch_3 s2(word_rd, word_rd_s, controller_clk, word_rd_r);
synch_3 s3(word_wr, word_wr_s, controller_clk, word_wr_r);

// Note: word_addr and word_data are NOT synchronized internally
// They must be stable when word_rd/word_wr asserts!
wire [23:0] word_addr_s;  // Defined but original doesn't use it
wire [31:0] word_data_s;  // Defined but original doesn't use it
```

## Request Queuing

When the FSM is busy, incoming requests are queued:

```verilog
// Catch incoming events if FSM is busy (lines 538-547)
if(word_rd_r) begin
    word_rd_queue <= 1;
    word_busy <= 1;        // Signal busy immediately
end
if(word_wr_r) begin
    word_wr_queue <= 1;
    word_busy <= 1;        // Signal busy immediately
end
```

## Auto-Refresh

The controller automatically issues refresh commands:

```verilog
// Refresh counter (lines 552-560)
refresh_count <= refresh_count + 1'b1;
if(&refresh_count) begin
    // Every ~6.144µs at 133MHz (fits 8192 rows in 64ms)
    refresh_count <= 0;
    issue_autorefresh <= 1;
end

// Refresh has priority in ST_IDLE
if(issue_autorefresh) begin
    state <= ST_REFRESH_0;
end
```

## Performance Characteristics

| Operation | Cycles (133 MHz) | Time |
|-----------|------------------|------|
| Word Read | ~12 cycles | ~90ns |
| Word Write | ~12 cycles | ~90ns |
| Burst Read (per word) | ~4 cycles | ~30ns |
| Refresh | ~10 cycles | ~75ns |

## Common Issues

1. **word_busy not asserted early enough**: Set `word_busy <= 1` when queuing request, not when starting operation.

2. **Address/data not stable**: Ensure `word_addr` and `word_data` are stable before asserting `word_rd`/`word_wr`.

3. **Byte ordering mismatch**: Read and write paths must use consistent ordering.

4. **Refresh starvation**: Long burst operations can delay refresh. Controller handles this internally.

## See Also

- [03-clock-domain-crossing.md](03-clock-domain-crossing.md) - CDC patterns
- [05-timing-analysis.md](05-timing-analysis.md) - Detailed timing
- [06-gotchas-pitfalls.md](06-gotchas-pitfalls.md) - Common mistakes
