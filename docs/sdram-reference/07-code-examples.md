# SDRAM Code Examples

## Overview

This document provides working code examples for SDRAM integration on the Analogue Pocket. Examples are drawn from AnaloguePicoRV32 and openfpga-litex projects.

## Example 1: Basic synch_3 Synchronizer

**Use for:** Single-bit control signals crossing clock domains

```verilog
// From common.v - 3-stage synchronizer with edge detection
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

// Edge detection (only for WIDTH == 1)
assign rise = (WIDTH == 1) ? (o & ~stage_3) : 1'b0;
assign fall = (WIDTH == 1) ? (~o & stage_3) : 1'b0;

always @(posedge clk)
    {stage_3, o, stage_2, stage_1} <= {o, stage_2, stage_1, i};

endmodule
```

**Usage:**
```verilog
wire cpu_rd_sync, cpu_rd_rise, cpu_rd_fall;
synch_3 sync_rd (
    .i(cpu_sdram_rd),
    .o(cpu_rd_sync),
    .clk(clk_74a),
    .rise(cpu_rd_rise),
    .fall(cpu_rd_fall)
);

// cpu_rd_rise is HIGH for exactly 1 clk_74a cycle on 0→1 transition
```

---

## Example 2: CPU to SDRAM Request (AnaloguePicoRV32 Pattern)

**Use for:** CPU requesting SDRAM access through bridge domain

### CPU Side (cpu_system.v)

```verilog
// State registers
reg mem_pending;
reg sdram_pending;
reg sdram_wait_busy;
reg sdram_rd;
reg sdram_wr;
reg [23:0] sdram_addr;
reg [31:0] sdram_wdata;

// Memory access state machine
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        mem_ready <= 0;
        mem_pending <= 0;
        sdram_pending <= 0;
        sdram_wait_busy <= 0;
        sdram_rd <= 0;
        sdram_wr <= 0;
    end else begin
        mem_ready <= 0;  // Default: not ready

        // START: New SDRAM access
        if (mem_valid && !mem_ready && !mem_pending && sdram_select) begin
            sdram_addr <= mem_addr[25:2];  // Word address
            if (|mem_wstrb) begin
                sdram_wr <= 1;
                sdram_wdata <= mem_wdata;
            end else begin
                sdram_rd <= 1;
            end
            mem_pending <= 1;
            sdram_pending <= 1;
            sdram_wait_busy <= 1;  // Phase 1: wait for busy HIGH
        end

        // WAIT states for SDRAM
        else if (mem_pending && sdram_pending) begin
            if (sdram_wait_busy) begin
                // Phase 1: Keep request until busy seen
                if (sdram_busy) begin
                    sdram_wait_busy <= 0;
                    sdram_rd <= 0;
                    sdram_wr <= 0;
                end
            end else if (!sdram_busy) begin
                // Phase 2: Operation complete
                mem_ready <= 1;
                mem_rdata <= sdram_rdata;
                mem_pending <= 0;
                sdram_pending <= 0;
            end
        end
    end
end
```

### Bridge Side (core_top.v)

```verilog
// Synchronize CPU signals to bridge domain
wire cpu_sdram_rd_sync, cpu_sdram_rd_rise;
wire cpu_sdram_wr_sync, cpu_sdram_wr_rise;

synch_3 sync_cpu_rd (
    .i(cpu_sdram_rd),
    .o(cpu_sdram_rd_sync),
    .clk(clk_74a),
    .rise(cpu_sdram_rd_rise)
);

synch_3 sync_cpu_wr (
    .i(cpu_sdram_wr),
    .o(cpu_sdram_wr_sync),
    .clk(clk_74a),
    .rise(cpu_sdram_wr_rise)
);

// Issue requests to SDRAM controller
always @(posedge clk_74a) begin
    ram1_word_rd <= 0;
    ram1_word_wr <= 0;

    // CPU access (when bridge not active)
    if (dataslot_allcomplete && !bridge_wr && !bridge_rd) begin
        if (cpu_sdram_rd_rise) begin
            ram1_word_rd <= 1;
            ram1_word_addr <= cpu_sdram_addr;
        end
        if (cpu_sdram_wr_rise) begin
            ram1_word_wr <= 1;
            ram1_word_addr <= cpu_sdram_addr;
            ram1_word_data <= cpu_sdram_wdata;
        end
    end
end

// Synchronize busy and read data back to CPU
reg [2:0] busy_sync;
reg [31:0] rdata_sync1, rdata_sync2;

always @(posedge clk_cpu) begin
    busy_sync <= {busy_sync[1:0], ram1_word_busy};
    rdata_sync1 <= ram1_word_q;
    rdata_sync2 <= rdata_sync1;
end

assign cpu_sdram_busy = busy_sync[2];
assign cpu_sdram_rdata = rdata_sync2;
```

---

## Example 3: Dual-Clock FIFO (openfpga-litex Pattern)

**Use for:** High-bandwidth, robust CDC for data transfer

### Request FIFO (CPU → SDRAM)

```verilog
// FIFO for SDRAM requests
wire req_fifo_empty;
wire [57:0] req_fifo_out;  // {wr_flag, addr[24:0], wdata[31:0]}

dcfifo request_fifo (
    // Write side (CPU domain)
    .wrclk(clk_cpu),
    .wrreq(cpu_sdram_rd | cpu_sdram_wr),
    .data({cpu_sdram_wr, cpu_sdram_addr, cpu_sdram_wdata}),
    .wrfull(req_full),

    // Read side (SDRAM controller domain)
    .rdclk(clk_74a),
    .rdreq(!req_fifo_empty && sdram_ready),
    .q(req_fifo_out),
    .rdempty(req_fifo_empty)
);

defparam request_fifo.intended_device_family = "Cyclone V",
         request_fifo.lpm_numwords = 4,
         request_fifo.lpm_showahead = "OFF",
         request_fifo.lpm_type = "dcfifo",
         request_fifo.lpm_width = 58,
         request_fifo.lpm_widthu = 2,
         request_fifo.overflow_checking = "ON",
         request_fifo.underflow_checking = "ON",
         request_fifo.rdsync_delaypipe = 5,
         request_fifo.wrsync_delaypipe = 5,
         request_fifo.use_eab = "ON";

// Extract fields from FIFO output
wire req_is_write = req_fifo_out[57];
wire [24:0] req_addr = req_fifo_out[56:32];
wire [31:0] req_wdata = req_fifo_out[31:0];
```

### Response FIFO (SDRAM → CPU)

```verilog
// FIFO for read responses
wire resp_fifo_empty;
wire [31:0] resp_fifo_out;

dcfifo response_fifo (
    // Write side (SDRAM controller domain)
    .wrclk(clk_74a),
    .wrreq(sdram_read_valid),
    .data(ram1_word_q),
    .wrfull(resp_full),

    // Read side (CPU domain)
    .rdclk(clk_cpu),
    .rdreq(!resp_fifo_empty),
    .q(resp_fifo_out),
    .rdempty(resp_fifo_empty)
);

defparam response_fifo.intended_device_family = "Cyclone V",
         response_fifo.lpm_numwords = 4,
         response_fifo.lpm_showahead = "OFF",
         response_fifo.lpm_type = "dcfifo",
         response_fifo.lpm_width = 32,
         response_fifo.lpm_widthu = 2,
         response_fifo.rdsync_delaypipe = 5,
         response_fifo.wrsync_delaypipe = 5;

// CPU reads from response FIFO
assign cpu_rdata = resp_fifo_out;
assign cpu_rdata_valid = !resp_fifo_empty;
```

---

## Example 4: SDRAM Word Read (io_sdram.v)

**Use for:** Understanding the SDRAM controller FSM

```verilog
// State machine for word read
localparam ST_IDLE = 0;
localparam ST_READ_0 = 10;
localparam ST_READ_1 = 11;
localparam ST_READ_2 = 12;
// ... more states

always @(posedge controller_clk) begin
    case (state)
        ST_IDLE: begin
            if (word_rd_queue) begin
                word_rd_queue <= 0;
                addr <= {word_addr, 1'b0};  // Convert to 16-bit address
                length <= 2;  // Read 2 x 16-bit = 32-bit word
                state <= ST_READ_0;
            end
        end

        ST_READ_0: begin
            // Activate row
            phy_ba <= addr[24:23];
            phy_a <= addr[22:10];
            cmd <= CMD_ACT;
            dc <= 0;
            state <= ST_READ_1;
        end

        ST_READ_1: begin
            // Wait for tRCD
            cmd <= CMD_NOP;
            if (dc == TIMING_ACT_RW - 1) begin
                state <= ST_READ_2;
            end
            dc <= dc + 1;
        end

        ST_READ_2: begin
            // Issue first READ command
            phy_a[12:11] <= 2'b00;
            phy_a[10] <= 1'b1;  // Auto-precharge
            phy_a[9:0] <= addr[9:0];
            cmd <= CMD_READ;
            enable_dq_read <= 1;
            length <= length - 1;
            addr <= addr + 1;
            state <= ST_READ_3;
        end

        ST_READ_3: begin
            // Issue second READ command
            phy_a[9:0] <= addr[9:0];
            cmd <= CMD_READ;
            length <= length - 1;
            state <= ST_READ_4;
        end

        // ST_READ_4-6: Wait for CAS latency, capture data
        // Data captured in separate always block with pipeline

        ST_READ_7: begin
            // Return to idle
            word_busy <= 0;
            state <= ST_IDLE;
        end
    endcase
end

// Data capture pipeline (CAS latency = 3)
reg enable_dq_read_1, enable_dq_read_2, enable_dq_read_3, enable_dq_read_4;
reg enable_dq_read_toggle;

always @(posedge controller_clk) begin
    enable_dq_read_1 <= enable_dq_read;
    enable_dq_read_2 <= enable_dq_read_1;
    enable_dq_read_3 <= enable_dq_read_2;
    enable_dq_read_4 <= enable_dq_read_3;

    if (enable_dq_read_4) begin
        if (~enable_dq_read_toggle) begin
            word_q[31:16] <= phy_dq;  // First read → high half
            enable_dq_read_toggle <= 1;
        end else begin
            word_q[15:0] <= phy_dq;   // Second read → low half
            enable_dq_read_toggle <= 0;
        end
    end
end
```

---

## Example 5: Memory Test Pattern (C Code)

**Use for:** Validating SDRAM functionality

```c
#include <stdint.h>

#define SDRAM_BASE 0x10000000
#define SDRAM_SIZE (64 * 1024 * 1024)  // 64MB

// Test pattern: address as data
int memtest_address_pattern(uint32_t base, uint32_t size) {
    volatile uint32_t *mem = (volatile uint32_t *)base;
    int errors = 0;

    // Write phase
    for (uint32_t i = 0; i < size / 4; i++) {
        mem[i] = base + (i * 4);  // Address as data
    }

    // Read/verify phase
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t expected = base + (i * 4);
        uint32_t actual = mem[i];

        if (actual != expected) {
            errors++;
            // XOR shows which bits are wrong
            uint32_t diff = actual ^ expected;
            printf("FAIL @ 0x%08X: exp=0x%08X got=0x%08X diff=0x%08X\n",
                   base + (i * 4), expected, actual, diff);

            if (errors > 100) {
                printf("Too many errors, stopping\n");
                return errors;
            }
        }
    }

    return errors;
}

// Test pattern: walking ones
int memtest_walking_ones(uint32_t base, uint32_t count) {
    volatile uint32_t *mem = (volatile uint32_t *)base;
    int errors = 0;

    for (uint32_t i = 0; i < count; i++) {
        for (int bit = 0; bit < 32; bit++) {
            uint32_t pattern = 1 << bit;

            mem[i] = pattern;
            uint32_t readback = mem[i];

            if (readback != pattern) {
                errors++;
                printf("Walking 1s FAIL @ 0x%08X bit %d: "
                       "wrote 0x%08X read 0x%08X\n",
                       base + (i * 4), bit, pattern, readback);
            }
        }
    }

    return errors;
}

// Test pattern: random with seed
uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int memtest_random(uint32_t base, uint32_t size, uint32_t seed) {
    volatile uint32_t *mem = (volatile uint32_t *)base;
    uint32_t state = seed;
    int errors = 0;

    // Write random values
    for (uint32_t i = 0; i < size / 4; i++) {
        mem[i] = xorshift32(&state);
    }

    // Reset RNG and verify
    state = seed;
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t expected = xorshift32(&state);
        uint32_t actual = mem[i];

        if (actual != expected) {
            errors++;
            printf("Random FAIL @ 0x%08X\n", base + (i * 4));
        }
    }

    return errors;
}
```

---

## Example 6: Wishbone Master (openfpga-litex)

**Use for:** Reference for Wishbone-based SDRAM access

```verilog
// Simplified Wishbone state machine
localparam NONE = 0;
localparam WRITE = 1;
localparam READ = 2;

reg [2:0] state;

always @(posedge clk_sys) begin
    sel <= 4'hF;  // All byte lanes
    cti <= 3'h0;  // Classic cycle (no burst)

    case (state)
        NONE: begin
            cyc <= 0;
            stb <= 0;
            we  <= 0;

            // Check for pending requests
            if (~write_fifo_empty) begin
                state <= WRITE;
                addr <= write_addr;
                data_write <= write_data;
            end else if (~read_fifo_empty) begin
                state <= READ;
                addr <= read_addr;
            end
        end

        WRITE: begin
            cyc <= 1;
            stb <= 1;
            we  <= 1;

            if (ack) begin
                state <= NONE;
                cyc <= 0;
                stb <= 0;
            end
        end

        READ: begin
            cyc <= 1;
            stb <= 1;
            we  <= 0;

            if (ack) begin
                // Capture read data
                read_data_out <= data_read;
                read_data_valid <= 1;
                state <= NONE;
                cyc <= 0;
                stb <= 0;
            end
        end
    endcase
end
```

---

## Example 7: Complete CDC Wrapper

**Use for:** Encapsulating all CDC logic in one module

```verilog
module sdram_cdc_wrapper (
    // CPU domain
    input  wire        clk_cpu,
    input  wire        cpu_rd,
    input  wire        cpu_wr,
    input  wire [23:0] cpu_addr,
    input  wire [31:0] cpu_wdata,
    output wire [31:0] cpu_rdata,
    output wire        cpu_busy,

    // SDRAM controller domain
    input  wire        clk_sdram,
    output wire        sdram_rd,
    output wire        sdram_wr,
    output wire [23:0] sdram_addr,
    output wire [31:0] sdram_wdata,
    input  wire [31:0] sdram_rdata,
    input  wire        sdram_busy
);

// CPU → SDRAM: Request synchronization
wire cpu_rd_sync, cpu_rd_rise;
wire cpu_wr_sync, cpu_wr_rise;

synch_3 sync_rd (
    .i(cpu_rd), .o(cpu_rd_sync), .clk(clk_sdram), .rise(cpu_rd_rise)
);

synch_3 sync_wr (
    .i(cpu_wr), .o(cpu_wr_sync), .clk(clk_sdram), .rise(cpu_wr_rise)
);

// Capture address and data on rising edge
reg [23:0] addr_captured;
reg [31:0] wdata_captured;
reg rd_pending, wr_pending;

always @(posedge clk_sdram) begin
    if (cpu_rd_rise) begin
        addr_captured <= cpu_addr;
        rd_pending <= 1;
    end
    if (cpu_wr_rise) begin
        addr_captured <= cpu_addr;
        wdata_captured <= cpu_wdata;
        wr_pending <= 1;
    end

    // Clear pending on busy rising edge
    if (sdram_busy) begin
        rd_pending <= 0;
        wr_pending <= 0;
    end
end

assign sdram_rd = rd_pending;
assign sdram_wr = wr_pending;
assign sdram_addr = addr_captured;
assign sdram_wdata = wdata_captured;

// SDRAM → CPU: Response synchronization
reg [31:0] rdata_sync1, rdata_sync2;
reg [2:0] busy_sync;

always @(posedge clk_cpu) begin
    rdata_sync1 <= sdram_rdata;
    rdata_sync2 <= rdata_sync1;
    busy_sync <= {busy_sync[1:0], sdram_busy};
end

assign cpu_rdata = rdata_sync2;
assign cpu_busy = busy_sync[2];

endmodule
```

---

## See Also

- [02-io-sdram-controller.md](02-io-sdram-controller.md) - Full FSM documentation
- [03-clock-domain-crossing.md](03-clock-domain-crossing.md) - CDC theory
- [06-gotchas-pitfalls.md](06-gotchas-pitfalls.md) - Common mistakes
