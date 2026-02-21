# Clock Domain Crossing (CDC) for SDRAM Access

## Overview

Clock Domain Crossing (CDC) is the most critical aspect of SDRAM integration on the Analogue Pocket. Signals must safely cross between:

- **clk_cpu** (~57 MHz) - CPU domain
- **clk_74a** (74.25 MHz) - APF bridge domain
- **clk_ram** (133 MHz) - SDRAM controller domain

Improper CDC causes **metastability**, leading to:
- Random bit flips
- Data corruption
- System hangs
- Intermittent failures

## Metastability Explained

When a signal changes close to a flip-flop's clock edge, the output can enter a **metastable state** - neither 0 nor 1 - before eventually resolving.

```
                Setup Time    Hold Time
                    ◄────►    ◄───►
                         │    │
Data:     ─────────────┐ │    │ ┌─────────
                       └─┼────┼─┘
                         │    │
Clock:    ─────────────/‾│‾‾‾‾│\──────────
                         │    │
                         ▼    ▼
                    Metastability Window
```

**MTBF (Mean Time Between Failures)** depends on:
- Clock frequency ratio
- Number of synchronization stages
- Flip-flop characteristics

## Synchronizer Implementations

### synch_2 (2-Stage Synchronizer)

**File:** `src/fpga/apf/common.v` (lines 37-56)

```verilog
module synch_2 #(parameter WIDTH = 1) (
    input  wire [WIDTH-1:0] i,     // Async input
    output reg  [WIDTH-1:0] o,     // Synchronized output
    input  wire             clk,   // Destination clock
    output wire             rise,  // Rising edge pulse
    output wire             fall   // Falling edge pulse
);

reg [WIDTH-1:0] stage_1;
reg [WIDTH-1:0] stage_2;

assign rise = (WIDTH == 1) ? (o & ~stage_2) : 1'b0;
assign fall = (WIDTH == 1) ? (~o & stage_2) : 1'b0;

always @(posedge clk)
   {stage_2, o, stage_1} <= {o, stage_1, i};
endmodule
```

**Latency:** 2 destination clock cycles
**Use case:** Fast signals where latency is critical

### synch_3 (3-Stage Synchronizer)

**File:** `src/fpga/apf/common.v` (lines 59-79)

```verilog
module synch_3 #(parameter WIDTH = 1) (
   input  wire [WIDTH-1:0] i,     // Async input
   output reg  [WIDTH-1:0] o,     // Synchronized output
   input  wire             clk,   // Destination clock
   output wire             rise,  // Rising edge pulse
   output wire             fall   // Falling edge pulse
);

reg [WIDTH-1:0] stage_1;
reg [WIDTH-1:0] stage_2;
reg [WIDTH-1:0] stage_3;

assign rise = (WIDTH == 1) ? (o & ~stage_3) : 1'b0;
assign fall = (WIDTH == 1) ? (~o & stage_3) : 1'b0;

always @(posedge clk)
   {stage_3, o, stage_2, stage_1} <= {o, stage_2, stage_1, i};
endmodule
```

**Latency:** 3 destination clock cycles
**Use case:** Status flags, control signals (most common choice)

### Edge Detection

Both synchronizers provide rising/falling edge detection:

```verilog
// For single-bit signals only (WIDTH == 1)
wire rise = o & ~stage_N;   // Current high, previous low
wire fall = ~o & stage_N;   // Current low, previous high

// Usage:
wire cpu_rd_sync, cpu_rd_rise;
synch_3 sync_rd(.i(cpu_rd), .o(cpu_rd_sync), .clk(clk_74a), .rise(cpu_rd_rise));

// cpu_rd_rise is HIGH for exactly ONE clk_74a cycle when cpu_rd transitions 0→1
```

## Dual-Clock FIFO (dcfifo)

For data transfer, **dcfifo** (Intel/Altera primitive) is the robust choice.

### How dcfifo Works

```
Write Domain                              Read Domain
(clk_74a)                                (clk_sys)
    │                                        │
    ▼                                        ▼
┌─────────┐   Gray-code   ┌─────────┐   ┌─────────┐
│  Write  │───────────────│  5-stage│───│  Read   │
│ Pointer │   Sync        │  Sync   │   │ Pointer │
└─────────┘               └─────────┘   └─────────┘
    │                                        │
    ▼                                        ▼
┌─────────────────────────────────────────────────┐
│                   FIFO RAM                       │
│              (Dual-Port Memory)                  │
└─────────────────────────────────────────────────┘
```

**Gray-code synchronization** ensures only 1 bit changes per clock, preventing multi-bit glitches.

### dcfifo Configuration (openfpga-litex pattern)

```verilog
dcfifo sdram_write_fifo (
    .wrclk(clk_74a),           // Write clock domain
    .rdclk(clk_sys),           // Read clock domain

    .wrreq(write_enable),      // Write request
    .data({addr, wdata}),      // 58-bit: 26-bit addr + 32-bit data

    .rdreq(~empty),            // Read request
    .q(fifo_out),              // Output data

    .wrempty(wr_empty),        // Empty flag (write domain)
    .rdempty(rd_empty),        // Empty flag (read domain)
    .wrfull(wr_full),          // Full flag (write domain)
    .rdfull(rd_full)           // Full flag (read domain)
);

defparam sdram_write_fifo.intended_device_family = "Cyclone V",
         sdram_write_fifo.lpm_numwords = 4,           // 4-entry FIFO
         sdram_write_fifo.lpm_showahead = "OFF",      // Registered output
         sdram_write_fifo.lpm_type = "dcfifo",
         sdram_write_fifo.lpm_width = 58,             // Data width
         sdram_write_fifo.lpm_widthu = 2,             // log2(4) = 2
         sdram_write_fifo.overflow_checking = "ON",
         sdram_write_fifo.underflow_checking = "ON",
         sdram_write_fifo.rdsync_delaypipe = 5,       // 5-stage read sync
         sdram_write_fifo.wrsync_delaypipe = 5,       // 5-stage write sync
         sdram_write_fifo.use_eab = "ON";             // Use embedded memory
```

### When to Use dcfifo vs synch_3

| Scenario | Recommended | Reason |
|----------|-------------|--------|
| Single control bit | synch_3 | Low latency, simple |
| Status flags | synch_3 | Rarely changes |
| Multi-bit bus (static) | synch_3 #(.WIDTH(N)) | All bits change together |
| Data payload | **dcfifo** | Safe multi-bit transfer |
| High-bandwidth stream | **dcfifo** | Buffering + flow control |
| Request/response | **dcfifo** | Decouples domains |

## CDC Patterns for SDRAM

### Pattern 1: synch_3 + Edge Detection (Simple)

Used in AnaloguePicoRV32:

```verilog
// core_top.v - CPU to bridge domain
wire cpu_sdram_rd_sync, cpu_sdram_rd_rise;
synch_3 sync_cpu_rd(cpu_sdram_rd, cpu_sdram_rd_sync, clk_74a, cpu_sdram_rd_rise);

// Capture address when rising edge detected
always @(posedge clk_74a) begin
    if (cpu_sdram_rd_rise) begin
        ram1_word_rd <= 1;
        ram1_word_addr <= cpu_sdram_addr;  // Must be stable!
    end else begin
        ram1_word_rd <= 0;
    end
end
```

**Requirements:**
1. `cpu_sdram_addr` must be stable BEFORE `cpu_sdram_rd` asserts
2. `cpu_sdram_rd` must stay HIGH long enough to be captured
3. Busy signal must be synchronized back to CPU

### Pattern 2: Held Request (Robust synch_3)

```verilog
// CPU holds request HIGH until acknowledged
// cpu_system.v
always @(posedge clk_cpu) begin
    if (sdram_pending && sdram_wait_busy) begin
        // Keep sdram_rd HIGH until busy is seen
        if (sdram_busy) begin
            sdram_rd <= 0;           // Release request
            sdram_wait_busy <= 0;    // Move to wait-for-complete
        end
        // else: keep sdram_rd asserted
    end
end
```

**Advantage:** Request stays asserted for many cycles, ensuring capture.

### Pattern 3: dcfifo (Most Robust)

Used in openfpga-litex:

```verilog
// Write request FIFO: CPU → SDRAM controller
dcfifo request_fifo (
    .wrclk(clk_cpu),
    .rdclk(clk_74a),
    .wrreq(cpu_sdram_rd | cpu_sdram_wr),
    .data({cpu_sdram_wr, cpu_sdram_addr, cpu_sdram_wdata}),
    .rdreq(~req_empty && sdram_ready),
    .q({is_write, req_addr, req_data}),
    .rdempty(req_empty)
);

// Read response FIFO: SDRAM controller → CPU
dcfifo response_fifo (
    .wrclk(clk_74a),
    .rdclk(clk_cpu),
    .wrreq(sdram_read_valid),
    .data(sdram_rdata),
    .rdreq(~resp_empty),
    .q(cpu_rdata),
    .rdempty(resp_empty)
);
```

**Advantages:**
- No timing assumptions about signal widths
- Automatic flow control via full/empty flags
- Decouples clock domains completely
- Handles back-pressure naturally

## Synchronizing Read Data Back to CPU

The read data path (SDRAM → CPU) also needs synchronization:

### Simple 2-Stage Sync

```verilog
// core_top.v
reg [31:0] sdram_rdata_sync1, sdram_rdata_sync2;
always @(posedge clk_cpu) begin
    sdram_rdata_sync1 <= ram1_word_q;
    sdram_rdata_sync2 <= sdram_rdata_sync1;
end
assign cpu_sdram_rdata = sdram_rdata_sync2;
```

**Note:** This works because by the time `busy` goes LOW (synchronized), the data has been stable for many cycles.

### dcfifo for Read Data

```verilog
dcfifo read_fifo (
    .wrclk(clk_ram),
    .rdclk(clk_cpu),
    .wrreq(word_read_complete),
    .data(word_q),
    .rdreq(cpu_reading),
    .q(cpu_rdata),
    .rdempty(read_empty)
);
```

## Busy Signal Synchronization

The `word_busy` signal crosses from SDRAM domain to CPU domain:

```verilog
// core_top.v
reg [2:0] sdram_busy_sync;
always @(posedge clk_cpu) begin
    sdram_busy_sync <= {sdram_busy_sync[1:0], ram1_word_busy};
end
assign cpu_sdram_busy = sdram_busy_sync[2];  // 3-cycle latency
```

**Critical timing:**
- CPU sees `busy` go HIGH: 3 CPU cycles after SDRAM sets it
- CPU sees `busy` go LOW: 3 CPU cycles after operation completes
- Total round-trip: 6+ cycles of synchronization latency

## Common CDC Mistakes

### Mistake 1: No Synchronization

```verilog
// WRONG: Direct assignment across clock domains
assign cpu_sdram_rdata = ram1_word_q;  // Metastability!
```

### Mistake 2: Single-Stage Sync

```verilog
// WRONG: Only 1 flip-flop
reg sync;
always @(posedge clk_dst) sync <= async_input;  // Still metastable!
```

### Mistake 3: Multi-Bit Bus Without FIFO

```verilog
// DANGEROUS: Each bit may resolve differently
synch_3 #(.WIDTH(32)) data_sync(.i(data_bus), .o(data_sync), .clk(clk));
// Only safe if all 32 bits change at exactly the same time
```

### Mistake 4: Pulse Too Short

```verilog
// PROBLEMATIC: 1-cycle pulse may be missed
always @(posedge clk_src) begin
    request <= 0;
    if (start) request <= 1;  // Only HIGH for 1 cycle!
end
```

## Timing Analysis Example

**Scenario:** CPU @ 57 MHz, Bridge @ 74.25 MHz, SDRAM @ 133 MHz

```
CPU cycle:    17.5 ns
Bridge cycle: 13.5 ns
SDRAM cycle:   7.5 ns

synch_3 latency (CPU → Bridge): 3 × 13.5 ns = 40.5 ns
synch_3 latency (Bridge → SDRAM): 3 × 7.5 ns = 22.5 ns

Total request latency: ~63 ns minimum
Plus SDRAM operation: ~90 ns
Plus return path sync: ~52.5 ns

Total round-trip: ~205 ns (~12 CPU cycles)
```

## Recommendations

1. **Use synch_3** for control signals (rd, wr, busy)
2. **Use dcfifo** for data if bandwidth allows
3. **Hold requests HIGH** until acknowledged
4. **Set busy immediately** when queuing, not when starting
5. **Synchronize read data** back to CPU domain
6. **Test at reduced clock speeds** to verify CDC is correct

## See Also

- [02-io-sdram-controller.md](02-io-sdram-controller.md) - Internal CDC in io_sdram.v
- [04-cpu-integration.md](04-cpu-integration.md) - CPU handshaking
- [06-gotchas-pitfalls.md](06-gotchas-pitfalls.md) - More CDC mistakes
- [07-code-examples.md](07-code-examples.md) - Working patterns
