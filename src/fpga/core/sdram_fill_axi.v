//
// SDRAM Fill Engine (AXI4 Master)
//
// Fills a contiguous SDRAM region with a constant 32-bit pattern.
// Operates as an AXI4 write-only master on the SDRAM arbiter.
//
// Register map (active on periph bus, directly exposed here):
//   0x00: FILL_ADDR    (RW) - Start byte address (word-aligned)
//   0x04: FILL_LENGTH  (RW) - Bytes to fill (must be multiple of 4)
//   0x08: FILL_DATA    (RW) - 32-bit fill pattern
//   0x0C: FILL_CTRL    (W)  - Write 1 to start
//   0x10: FILL_STATUS  (R)  - bit0=busy
//

`default_nettype none

module sdram_fill_axi (
    input wire        clk,
    input wire        reset_n,

    // CPU register interface (active on periph bus write)
    input wire        reg_wr,
    input wire [4:0]  reg_addr,   // byte_offset[6:2]
    input wire [31:0] reg_wdata,
    output reg [31:0] reg_rdata,

    // AXI4 write master (to SDRAM arbiter)
    output reg         m_awvalid,
    input  wire        m_awready,
    output reg  [31:0] m_awaddr,
    output wire [7:0]  m_awlen,
    output reg         m_wvalid,
    input  wire        m_wready,
    output reg  [31:0] m_wdata,
    output wire [3:0]  m_wstrb,
    output wire        m_wlast,
    input  wire        m_bvalid,
    input  wire [1:0]  m_bresp,

    // Read channel (unused — tie off)
    output wire        m_arvalid,
    output wire [31:0] m_araddr,
    output wire [7:0]  m_arlen,

    // Status
    output wire        active
);

    // Single-beat writes only
    assign m_awlen  = 8'd0;
    assign m_wstrb  = 4'b1111;
    assign m_wlast  = 1'b1;

    // No reads
    assign m_arvalid = 1'b0;
    assign m_araddr  = 32'd0;
    assign m_arlen   = 8'd0;

    // FSM states
    localparam [2:0] ST_IDLE    = 3'd0;
    localparam [2:0] ST_AW      = 3'd1;  // Issue AW
    localparam [2:0] ST_W       = 3'd2;  // Issue W (may overlap with AW)
    localparam [2:0] ST_B_WAIT  = 3'd3;  // Wait for B response
    localparam [2:0] ST_NEXT    = 3'd4;  // Advance to next word

    reg [2:0]  state;
    reg [31:0] fill_addr_reg;
    reg [31:0] fill_length_reg;
    reg [31:0] fill_data_reg;

    reg [31:0] cur_addr;
    reg [31:0] remaining;
    reg        aw_done;  // AW accepted this beat
    reg        w_done;   // W accepted this beat

    assign active = (state != ST_IDLE);

    // Register read mux
    always @(*) begin
        case (reg_addr[2:0])
            3'd0: reg_rdata = fill_addr_reg;
            3'd1: reg_rdata = fill_length_reg;
            3'd2: reg_rdata = fill_data_reg;
            3'd3: reg_rdata = 32'd0;
            3'd4: reg_rdata = {31'd0, active};
            default: reg_rdata = 32'd0;
        endcase
    end

    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state          <= ST_IDLE;
            fill_addr_reg  <= 32'd0;
            fill_length_reg <= 32'd0;
            fill_data_reg  <= 32'd0;
            cur_addr       <= 32'd0;
            remaining      <= 32'd0;
            m_awvalid      <= 1'b0;
            m_awaddr       <= 32'd0;
            m_wvalid       <= 1'b0;
            m_wdata        <= 32'd0;
            aw_done        <= 1'b0;
            w_done         <= 1'b0;
        end else begin
            // Register writes
            if (reg_wr) begin
                case (reg_addr[2:0])
                    3'd0: fill_addr_reg   <= reg_wdata;
                    3'd1: fill_length_reg <= reg_wdata;
                    3'd2: fill_data_reg   <= reg_wdata;
                    3'd3: begin
                        if (reg_wdata[0] && state == ST_IDLE && fill_length_reg != 0) begin
                            cur_addr  <= fill_addr_reg;
                            remaining <= fill_length_reg;
                            state     <= ST_AW;
                        end
                    end
                    default: ;
                endcase
            end

            case (state)
                ST_IDLE: begin
                    m_awvalid <= 1'b0;
                    m_wvalid  <= 1'b0;
                end

                ST_AW: begin
                    // Issue AW and W simultaneously
                    m_awvalid <= 1'b1;
                    m_awaddr  <= cur_addr;
                    m_wvalid  <= 1'b1;
                    m_wdata   <= fill_data_reg;
                    aw_done   <= 1'b0;
                    w_done    <= 1'b0;
                    state     <= ST_W;
                end

                ST_W: begin
                    // Track AW and W acceptance independently
                    if (m_awready && m_awvalid) begin
                        m_awvalid <= 1'b0;
                        aw_done <= 1'b1;
                    end
                    if (m_wready && m_wvalid) begin
                        m_wvalid <= 1'b0;
                        w_done <= 1'b1;
                    end

                    // Both accepted — wait for B
                    if ((aw_done || (m_awready && m_awvalid)) &&
                        (w_done  || (m_wready  && m_wvalid))) begin
                        m_awvalid <= 1'b0;
                        m_wvalid  <= 1'b0;
                        state <= ST_B_WAIT;
                    end
                end

                ST_B_WAIT: begin
                    if (m_bvalid) begin
                        state <= ST_NEXT;
                    end
                end

                ST_NEXT: begin
                    cur_addr  <= cur_addr + 32'd4;
                    remaining <= remaining - 32'd4;
                    if (remaining <= 32'd4)
                        state <= ST_IDLE;
                    else
                        state <= ST_AW;
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

endmodule
