# CPU Integration with SDRAM

## Overview

This document covers how soft CPUs (PicoRV32, VexRiscv) integrate with SDRAM on the Analogue Pocket, including memory interfaces, handshaking protocols, and implementation patterns.

## PicoRV32 Memory Interface

**File:** `src/fpga/core/cpu_system.v`

### Interface Signals

```verilog
// PicoRV32 native memory interface
wire        mem_valid;    // CPU asserts when accessing memory
wire        mem_instr;    // 1 = instruction fetch, 0 = data access
reg         mem_ready;    // Peripheral asserts when data ready
wire [31:0] mem_addr;     // Byte address
wire [31:0] mem_wdata;    // Write data
wire [3:0]  mem_wstrb;    // Write strobe (byte enables)
reg  [31:0] mem_rdata;    // Read data
```

### Memory Map Decoding

```verilog
// Address decode (cpu_system.v, lines 82-86)
wire ram_select    = (mem_addr[31:16] == 16'b0);           // 0x00000000-0x0000FFFF (64KB BRAM)
wire sdram_select  = (mem_addr[31:26] == 6'b000100) ||     // 0x10000000-0x13FFFFFF
                     (mem_addr[31:26] == 6'b000101);       // 0x14000000-0x17FFFFFF
wire term_select   = (mem_addr[31:13] == 19'h10000);       // 0x20000000-0x20001FFF
wire sysreg_select = (mem_addr[31:5] == 27'h2000000);      // 0x40000000-0x4000001F
```

## SDRAM Access State Machine

The CPU uses a state machine to manage SDRAM accesses:

```verilog
// State registers
reg mem_pending;       // Memory access in progress
reg sdram_pending;     // SDRAM access specifically
reg sdram_wait_busy;   // Waiting for busy to go HIGH

// Output signals to SDRAM
reg         sdram_rd;
reg         sdram_wr;
reg  [23:0] sdram_addr;
reg  [31:0] sdram_wdata;

// Input signals from SDRAM (synchronized)
wire [31:0] sdram_rdata;
wire        sdram_busy;
```

### Access Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                    SDRAM ACCESS STATE MACHINE                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   IDLE ──────► START ──────► WAIT_BUSY_HIGH ──────► WAIT_BUSY_LOW   │
│     ▲                                                      │         │
│     │                                                      ▼         │
│     └──────────────────────── COMPLETE ◄───────────────────┘         │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation

```verilog
// cpu_system.v (lines 195-260)
always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        // Reset state
        mem_ready <= 0;
        mem_pending <= 0;
        sdram_pending <= 0;
        sdram_wait_busy <= 0;
        sdram_rd <= 0;
        sdram_wr <= 0;
    end else begin
        mem_ready <= 0;

        // START: New memory access
        if (mem_valid && !mem_ready && !mem_pending) begin
            if (sdram_select) begin
                // Setup SDRAM access
                sdram_addr <= mem_addr[25:2];  // Word address
                if (|mem_wstrb) begin
                    sdram_wr <= 1;
                    sdram_wdata <= mem_wdata;
                end else begin
                    sdram_rd <= 1;
                end
                mem_pending <= 1;
                sdram_pending <= 1;
                sdram_wait_busy <= 1;  // First wait for busy HIGH
            end
            // ... other peripherals ...
        end

        // WAIT states
        else if (mem_pending && sdram_pending) begin
            if (sdram_wait_busy) begin
                // WAIT_BUSY_HIGH: Keep request asserted until busy seen
                if (sdram_busy) begin
                    sdram_wait_busy <= 0;
                    sdram_rd <= 0;   // Release request
                    sdram_wr <= 0;
                end
            end else if (!sdram_busy) begin
                // WAIT_BUSY_LOW: Operation complete
                mem_ready <= 1;
                mem_rdata <= sdram_rdata;
                mem_pending <= 0;
                sdram_pending <= 0;
            end
        end
    end
end
```

## Handshaking Protocol

### Two-Phase Handshake

The AnaloguePicoRV32 uses a two-phase handshake:

```
Phase 1: Wait for busy HIGH (request acknowledged)
Phase 2: Wait for busy LOW (operation complete)

CPU Domain          Bridge Domain        SDRAM Domain
    │                    │                    │
    │  sdram_rd=1        │                    │
    ├───────────────────►│  word_rd pulse     │
    │                    ├───────────────────►│
    │                    │                    │ word_busy=1
    │  sdram_busy=1      │◄───────────────────┤
    │◄───────────────────┤                    │
    │  sdram_rd=0        │                    │
    │                    │                    │ (SDRAM op)
    │                    │                    │
    │                    │                    │ word_busy=0
    │  sdram_busy=0      │◄───────────────────┤
    │◄───────────────────┤                    │
    │  mem_ready=1       │                    │
    │                    │                    │
```

### Why Two Phases?

1. **Phase 1 ensures the request was received** across clock domain crossing
2. **Phase 2 waits for actual completion** of SDRAM operation
3. Without Phase 1, the CPU might see `busy=0` before `busy=1` due to sync latency

## VexRiscv + Wishbone (openfpga-litex)

The openfpga-litex project uses VexRiscv with Wishbone bus interface:

### Wishbone Signals

```verilog
// Wishbone master interface
output wire [29:0] addr,        // Word address (30-bit)
output reg  [1:0]  bte,         // Burst type extension
output reg  [2:0]  cti,         // Cycle type identifier
output reg         cyc,         // Cycle active
output wire [31:0] data_write,  // Write data
output reg  [3:0]  sel,         // Byte select
output reg         stb,         // Strobe (valid address/data)
output reg         we,          // Write enable
input wire         ack,         // Acknowledge
input wire [31:0]  data_read,   // Read data
input wire         err          // Error
```

### Wishbone State Machine

```verilog
// apf_wishbone_master.sv (lines 172-230)
localparam NONE = 0;
localparam WRITE = 1;
localparam READ = 2;

reg [2:0] state = 3'h0;

always @(posedge clk_sys) begin
    sel <= 4'hF;  // All bytes
    cti <= 3'h0;  // Linear burst

    case (state)
        NONE: begin
            cyc <= 0;
            stb <= 0;
            we  <= 0;

            if (~mem_write_empty) begin
                state <= WRITE;
            end else if (~addr_read_empty) begin
                state <= READ;
            end
        end

        WRITE: begin
            cyc <= 1;
            stb <= 1;
            we  <= 1;

            if (ack) begin
                state <= NONE;
                cyc   <= 0;
                stb   <= 0;
            end
        end

        READ: begin
            cyc <= 1;
            stb <= 1;
            we  <= 0;

            if (ack) begin
                state <= NONE;
                cyc   <= 0;
                stb   <= 0;
            end
        end
    endcase
end
```

### Wishbone vs Direct Interface

| Aspect | Direct (PicoRV32) | Wishbone (VexRiscv) |
|--------|-------------------|---------------------|
| Complexity | Simple | Standard protocol |
| Handshake | busy/ready | cyc/stb/ack |
| Burst support | No | Yes (cti/bte) |
| Error handling | None | err signal |
| Multi-master | Manual | Built-in arbitration |

## Bridge Integration (core_top.v)

The bridge in `core_top.v` multiplexes between APF bridge and CPU:

```verilog
// core_top.v (lines 337-389)
always @(posedge clk_74a) begin
    ram1_word_rd <= 0;
    ram1_word_wr <= 0;

    // APF Bridge write (priority)
    if(bridge_wr) begin
        casex(bridge_addr[31:24])
        8'b000000xx: begin
            ram1_word_wr <= 1;
            ram1_word_addr <= bridge_addr[25:2];
            ram1_word_data <= bridge_wr_data;
        end
        endcase
    end

    // APF Bridge read
    if(bridge_rd) begin
        casex(bridge_addr[31:24])
        8'b000000xx: begin
            ram1_word_rd <= 1;
            ram1_word_addr <= bridge_addr[25:2];
            ram1_bridge_rd_data <= ram1_word_q;  // Return previous read
        end
        endcase
    end

    // CPU access (when bridge not active)
    if(dataslot_allcomplete && !bridge_wr && !bridge_rd) begin
        if(cpu_sdram_rd_rise) begin
            ram1_word_rd <= 1;
            ram1_word_addr <= cpu_sdram_addr;
        end
        if(cpu_sdram_wr_rise) begin
            ram1_word_wr <= 1;
            ram1_word_addr <= cpu_sdram_addr;
            ram1_word_data <= cpu_sdram_wdata;
        end
    end
end
```

### Priority Scheme

1. **APF Bridge** has priority (for data slot loading)
2. **CPU** only accesses when `dataslot_allcomplete` and bridge idle
3. **No arbitration** - bridge always wins if active

## Byte Strobes and Partial Writes

PicoRV32 supports byte-granular writes via `mem_wstrb`:

```verilog
// mem_wstrb encoding:
// 4'b0001 = byte 0 (bits 7:0)
// 4'b0010 = byte 1 (bits 15:8)
// 4'b0100 = byte 2 (bits 23:16)
// 4'b1000 = byte 3 (bits 31:24)
// 4'b1111 = full word write
```

**Note:** SDRAM controller doesn't support partial writes - it writes full 32-bit words. For partial writes, you'd need read-modify-write:

```verilog
// Pseudocode for byte write to SDRAM:
// 1. Read current word
// 2. Modify selected bytes
// 3. Write back full word
```

## Performance Considerations

### Access Latency

```
BRAM access:     1 cycle (~17 ns @ 57 MHz)
SDRAM access:    ~12 cycles (~210 ns total round-trip)
                 - Request CDC: ~40 ns
                 - SDRAM operation: ~90 ns
                 - Response CDC: ~80 ns
```

### Optimization Strategies

1. **Keep hot data in BRAM** (stack, frequently accessed variables)
2. **Use burst reads** for sequential data (model weights)
3. **Minimize SDRAM writes** (heap allocator overhead)
4. **Consider caching** for read-heavy workloads

## Debugging Tips

### Signals to Monitor

1. `mem_valid` / `mem_ready` - CPU memory interface
2. `sdram_rd` / `sdram_wr` - SDRAM request signals
3. `sdram_busy` - SDRAM operation status
4. `sdram_pending` / `sdram_wait_busy` - State machine state

### Common Issues

1. **Stuck waiting for busy HIGH**: Request not crossing CDC
2. **Stuck waiting for busy LOW**: SDRAM controller hung
3. **Wrong data read**: Byte ordering mismatch
4. **Intermittent failures**: Metastability (CDC issues)

## See Also

- [02-io-sdram-controller.md](02-io-sdram-controller.md) - SDRAM controller details
- [03-clock-domain-crossing.md](03-clock-domain-crossing.md) - CDC patterns
- [06-gotchas-pitfalls.md](06-gotchas-pitfalls.md) - Common mistakes
