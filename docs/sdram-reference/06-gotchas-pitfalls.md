# SDRAM Gotchas and Pitfalls

## Overview

This document catalogs common mistakes when working with SDRAM on the Analogue Pocket. Each pitfall includes symptoms, root cause, and solution.

## Clock Domain Crossing (CDC) Pitfalls

### Pitfall 1: No Synchronization

**Symptom:** Random data corruption, intermittent hangs

**Wrong:**
```verilog
// Direct assignment across clock domains
assign cpu_sdram_rdata = ram1_word_q;  // METASTABILITY!
```

**Right:**
```verilog
// 2-stage synchronizer for read data
reg [31:0] rdata_sync1, rdata_sync2;
always @(posedge clk_cpu) begin
    rdata_sync1 <= ram1_word_q;
    rdata_sync2 <= rdata_sync1;
end
assign cpu_sdram_rdata = rdata_sync2;
```

**Why:** Signals crossing clock domains can be sampled during transitions, causing metastable states that resolve to random values.

---

### Pitfall 2: Single-Stage Synchronizer

**Symptom:** Reduced MTBF, occasional bit flips under load

**Wrong:**
```verilog
// Only 1 flip-flop - still metastable!
reg sync;
always @(posedge clk_dst) sync <= async_input;
```

**Right:**
```verilog
// Minimum 2 stages, 3 preferred for safety
reg sync1, sync2, sync3;
always @(posedge clk_dst) begin
    sync1 <= async_input;
    sync2 <= sync1;
    sync3 <= sync2;
end
```

**Why:** One flip-flop reduces metastability probability but doesn't eliminate it. Each stage adds ~100x MTBF improvement.

---

### Pitfall 3: Multi-Bit Bus Without FIFO

**Symptom:** Corrupted data values, bits from different samples

**Wrong:**
```verilog
// Each bit may resolve at different times!
synch_3 #(.WIDTH(32)) data_sync(
    .i(data_bus),
    .o(data_sync),
    .clk(clk)
);
```

**Right:**
```verilog
// Use FIFO for multi-bit data
dcfifo data_fifo (
    .wrclk(clk_src),
    .rdclk(clk_dst),
    .data(data_bus),
    .q(data_out),
    ...
);
```

**When synch_3 with WIDTH > 1 IS safe:**
- All bits change simultaneously (e.g., counter with Gray encoding)
- Signal is stable for many cycles before sampling
- Only need "eventual consistency" (status flags)

---

### Pitfall 4: Pulse Too Short to Cross

**Symptom:** Missed requests, operations never start

**Wrong:**
```verilog
// 1-cycle pulse may be missed entirely!
always @(posedge clk_src) begin
    request <= 0;
    if (start) request <= 1;  // Only HIGH for 1 cycle
end
```

**Right:**
```verilog
// Hold request until acknowledged
always @(posedge clk_src) begin
    if (start) request <= 1;
    if (ack_synced) request <= 0;  // Clear on ack
end
```

**Why:** A 1-cycle pulse at 57 MHz (17.5ns) might fall between two sampling edges of a 74 MHz clock (13.5ns period), especially after synchronizer latency.

---

### Pitfall 5: Not Waiting for Busy HIGH

**Symptom:** CPU sees stale busy=0, completes before SDRAM starts

**Wrong:**
```verilog
// Check busy immediately
if (sdram_rd) begin
    if (!sdram_busy) begin
        mem_ready <= 1;  // WRONG: busy hasn't risen yet!
    end
end
```

**Right:**
```verilog
// Two-phase handshake
if (sdram_wait_busy) begin
    // Phase 1: Wait for busy to go HIGH
    if (sdram_busy) begin
        sdram_wait_busy <= 0;
        sdram_rd <= 0;
    end
end else if (!sdram_busy) begin
    // Phase 2: Wait for busy to go LOW
    mem_ready <= 1;
end
```

**Why:** Synchronizer latency means `busy` appears LOW for 3+ cycles after request. CPU must wait to see `busy` go HIGH before waiting for LOW.

---

## SDRAM Controller Pitfalls

### Pitfall 6: Byte Ordering Mismatch

**Symptom:** Data appears swapped or scrambled

**Wrong (inconsistent ordering):**
```verilog
// Write: low half first
ST_WRITE_2: phy_dq_out <= word_data[15:0];
ST_WRITE_3: phy_dq_out <= word_data[31:16];

// Read: high half first (MISMATCH!)
word_q[31:16] <= first_read;
word_q[15:0] <= second_read;
```

**Right (consistent ordering):**
```verilog
// Write: high half first
ST_WRITE_2: phy_dq_out <= word_data[31:16];  // First
ST_WRITE_3: phy_dq_out <= word_data[15:0];   // Second

// Read: high half first (MATCHES!)
word_q[31:16] <= first_read;
word_q[15:0] <= second_read;
```

**Why:** SDRAM stores 16-bit values at sequential addresses. Reading in different order than writing scrambles the 32-bit word.

---

### Pitfall 7: Not Setting Busy Immediately

**Symptom:** Multiple requests queued, data overwritten

**Wrong:**
```verilog
// Busy only set when operation starts
if (word_rd_r) begin
    state <= ST_READ_0;
    word_busy <= 1;  // Too late if another request comes!
end
```

**Right:**
```verilog
// Busy set immediately on request detection
if (word_rd_r) begin
    word_rd_queue <= 1;
    word_busy <= 1;  // Block new requests immediately
end
```

**Why:** Between request detection and FSM start, another request could overwrite the address/data registers.

---

### Pitfall 8: Refresh Starvation

**Symptom:** Data corruption after long bursts, SDRAM lockup

**Wrong:**
```verilog
// Refresh only checked in IDLE
if (state == ST_IDLE && issue_autorefresh) begin
    state <= ST_REFRESH_0;
end
// Long burst blocks refresh for too long!
```

**Right:**
```verilog
// Track refresh urgency, interrupt long operations
if (refresh_urgent) begin
    // Force refresh even during burst
    state <= ST_REFRESH_0;
end
```

**Why:** SDRAM requires refresh every 7.8µs. Long bursts can exceed this, causing data loss in unrefreshed rows.

---

### Pitfall 9: Address Bit Mapping

**Symptom:** Wrong data returned, partial memory visible

**Wrong:**
```verilog
// Missing address bits
phy_a <= addr[9:0];  // Only 10 bits, missing upper address
```

**Right:**
```verilog
// Row activation (13 bits)
ST_READ_0: phy_a <= addr[22:10];

// Column read (10 bits)
ST_READ_2: phy_a <= addr[9:0];

// Bank select (2 bits)
phy_ba <= addr[24:23];
```

**Address breakdown for AS4C32M16:**
```
word_addr[24:0]:
  [24:23] = Bank (2 bits) → 4 banks
  [22:10] = Row (13 bits) → 8192 rows
  [9:0]   = Column (10 bits) → 1024 columns (×16 bits)
```

---

### Pitfall 10: Timing Violations

**Symptom:** Intermittent failures, works at lower speed

**Wrong:**
```verilog
// Insufficient tRCD
ST_READ_0: cmd <= CMD_ACT;
ST_READ_1: cmd <= CMD_READ;  // Only 1 cycle! Need 3.
```

**Right:**
```verilog
// Proper tRCD delay
ST_READ_0: begin
    cmd <= CMD_ACT;
    dc <= 0;
    state <= ST_READ_1;
end
ST_READ_1: begin
    cmd <= CMD_NOP;
    if (dc == TIMING_ACT_RW - 1) begin  // Wait 3 cycles
        state <= ST_READ_2;
    end
    dc <= dc + 1;
end
ST_READ_2: cmd <= CMD_READ;
```

---

## CPU Integration Pitfalls

### Pitfall 11: Address Not Stable

**Symptom:** Wrong address read, random memory locations

**Wrong:**
```verilog
// Address changes before capture
cpu_addr <= new_address;
cpu_rd <= 1;  // Address may not be stable yet!
```

**Right:**
```verilog
// Setup address first, then assert request
cpu_addr <= new_address;
// Next cycle:
cpu_rd <= 1;  // Address now stable
```

**Why:** Address must be stable BEFORE the rising edge that will be captured by the synchronizer.

---

### Pitfall 12: Early Ready Signal

**Symptom:** CPU reads stale data, continues before data arrives

**Wrong:**
```verilog
// Ready asserted before data synchronized
if (!sdram_busy) begin
    mem_ready <= 1;
    mem_rdata <= sdram_rdata;  // Data might not be stable!
end
```

**Right:**
```verilog
// Wait for synchronized data
if (!sdram_busy) begin
    // Data is stable (busy LOW means operation complete)
    mem_ready <= 1;
    mem_rdata <= sdram_rdata_sync;  // Use synchronized data
end
```

---

### Pitfall 13: Partial Write Assumption

**Symptom:** Byte writes corrupt adjacent bytes

**Wrong assumption:**
```verilog
// Trying byte write directly to SDRAM
if (mem_wstrb == 4'b0001) begin
    sdram_write(addr, byte_data);  // SDRAM writes 16 bits minimum!
end
```

**Right (read-modify-write):**
```verilog
// For byte write: read, modify, write
if (|mem_wstrb && mem_wstrb != 4'b1111) begin
    // 1. Read current word
    // 2. Modify selected bytes
    // 3. Write back full word
end
```

**Note:** io_sdram.v only supports full 32-bit word writes. Byte granularity requires read-modify-write cycle.

---

## Debugging Strategies

### Strategy 1: Reduce Clock Speed

```verilog
// Test at half speed
// In PLL configuration:
// clk_ram: 133 MHz → 66.5 MHz
// clk_cpu: 57 MHz → 28.5 MHz

// If works at half speed: timing issue
// If still fails: logic bug
```

### Strategy 2: Add Visible Indicators

```verilog
// LED indicators for state machine
assign led[0] = (state == ST_IDLE);
assign led[1] = word_busy;
assign led[2] = sdram_rd | sdram_wr;
assign led[3] = |error_count;
```

### Strategy 3: Memory Pattern Test

```c
// Write pattern
for (uint32_t i = 0; i < size; i += 4) {
    *(volatile uint32_t *)(base + i) = base + i;  // Address as data
}

// Verify pattern
for (uint32_t i = 0; i < size; i += 4) {
    uint32_t expected = base + i;
    uint32_t actual = *(volatile uint32_t *)(base + i);
    if (actual != expected) {
        // Log: address, expected, actual
        // XOR shows which bits differ
    }
}
```

### Strategy 4: SignalTap Capture

```verilog
// Critical signals to capture
.probe(state),
.probe(mem_valid),
.probe(mem_ready),
.probe(sdram_rd),
.probe(sdram_busy),
.probe(cpu_sdram_addr),
.probe(cpu_sdram_rdata),

// Trigger on error condition
.trigger(mem_valid && !mem_ready && timeout)
```

---

## Quick Reference: CDC Rules

| Rule | Description |
|------|-------------|
| **Always synchronize** | Every signal crossing domains needs 2+ FF stages |
| **Use 3 stages** | synch_3 for control signals (better MTBF) |
| **Use FIFO for data** | dcfifo for multi-bit data transfer |
| **Hold until ACK** | Keep request HIGH until acknowledged |
| **Two-phase handshake** | Wait for busy HIGH, then wait for busy LOW |
| **Stable before strobe** | Data/address stable before control signal |
| **Check both edges** | Verify rising AND falling edge detection |

---

## See Also

- [03-clock-domain-crossing.md](03-clock-domain-crossing.md) - CDC theory and patterns
- [04-cpu-integration.md](04-cpu-integration.md) - Handshaking protocols
- [05-timing-analysis.md](05-timing-analysis.md) - SDRAM timing parameters
- [07-code-examples.md](07-code-examples.md) - Working implementations
