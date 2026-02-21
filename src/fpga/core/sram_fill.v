//
// SRAM Fill Engine
// Simple sequential fill for z-buffer clear via SRAM controller.
//
// Register map (reg_addr = byte_offset[6:2]):
//   0x00: FILL_DST_ADDR  (RW) - Destination CPU byte address (SRAM-mapped)
//   0x04: FILL_LENGTH    (RW) - Transfer length in bytes (4-byte aligned)
//   0x08: FILL_DATA      (RW) - 32-bit fill pattern
//   0x0C: FILL_CONTROL   (W)  - Write 1 to start fill
//   0x10: FILL_STATUS    (R)  - bit0=busy
//

`default_nettype none

module sram_fill (
    input wire        clk,
    input wire        reset_n,

    // CPU register interface
    input wire        reg_wr,
    input wire [4:0]  reg_addr,
    input wire [31:0] reg_wdata,
    output reg [31:0] reg_rdata,

    // SRAM controller word interface
    output reg        word_wr,
    output reg [15:0] word_addr,
    output reg [31:0] word_data,
    output reg [3:0]  word_wstrb,
    input wire        word_busy,

    // Status
    output wire       active
);

    localparam [1:0] ST_IDLE       = 2'd0;
    localparam [1:0] ST_ISSUE      = 2'd1;
    localparam [1:0] ST_WAIT       = 2'd2;

    reg [1:0]  state;
    reg [31:0] dst_addr_reg;
    reg [31:0] length_reg;
    reg [31:0] fill_data_reg;

    reg [15:0] cur_addr;
    reg [31:0] remaining;
    reg        seen_busy;

    assign active = (state != ST_IDLE);

    // Register read mux
    always @(*) begin
        case (reg_addr[2:0])
            3'd0: reg_rdata = dst_addr_reg;
            3'd1: reg_rdata = length_reg;
            3'd2: reg_rdata = fill_data_reg;
            3'd3: reg_rdata = 32'd0;  // CONTROL (write-only)
            3'd4: reg_rdata = {31'd0, active};
            default: reg_rdata = 32'd0;
        endcase
    end

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state         <= ST_IDLE;
            dst_addr_reg  <= 32'd0;
            length_reg    <= 32'd0;
            fill_data_reg <= 32'd0;
            cur_addr      <= 16'd0;
            remaining     <= 32'd0;
            seen_busy     <= 1'b0;
            word_wr       <= 1'b0;
            word_addr     <= 16'd0;
            word_data     <= 32'd0;
            word_wstrb    <= 4'b0;
        end else begin
            word_wr <= 1'b0;

            // Register writes
            if (reg_wr) begin
                case (reg_addr[2:0])
                    3'd0: dst_addr_reg  <= reg_wdata;
                    3'd1: length_reg    <= reg_wdata;
                    3'd2: fill_data_reg <= reg_wdata;
                    3'd3: begin
                        // CONTROL: start fill
                        if (reg_wdata[0] && state == ST_IDLE) begin
                            cur_addr  <= dst_addr_reg[17:2];
                            remaining <= length_reg;
                            state     <= ST_ISSUE;
                        end
                    end
                    default: ;
                endcase
            end

            // Fill state machine
            case (state)
                ST_IDLE: ;

                ST_ISSUE: begin
                    if (!word_busy) begin
                        word_wr    <= 1'b1;
                        word_addr  <= cur_addr;
                        word_data  <= fill_data_reg;
                        word_wstrb <= 4'b1111;
                        seen_busy  <= 1'b0;
                        state      <= ST_WAIT;
                    end
                end

                ST_WAIT: begin
                    if (word_busy)
                        seen_busy <= 1'b1;

                    if (seen_busy && !word_busy) begin
                        cur_addr  <= cur_addr + 16'd1;
                        remaining <= remaining - 32'd4;

                        if (remaining <= 32'd4)
                            state <= ST_IDLE;
                        else
                            state <= ST_ISSUE;
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
