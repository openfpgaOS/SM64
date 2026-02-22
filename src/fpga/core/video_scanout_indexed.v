//
// Video Scanout with 8-bit Indexed Color and Hardware Palette
// Reads 8-bit palette indices from BRAM framebuffer (160x120),
// performs 2x pixel/line replication to 320x240, looks up RGB888 in palette RAM.
//

`default_nettype none

module video_scanout_indexed (
    // Video clock domain (12.288 MHz)
    input wire clk_video,
    input wire reset_n,

    // Video timing inputs
    input wire [9:0] x_count,
    input wire [9:0] y_count,

    // Pixel output (RGB888)
    output reg [23:0] pixel_color,

    // BRAM framebuffer read port (directly accessible, dual-port BRAM in cpu_system)
    output wire [12:0] fb_rd_addr,    // Word address into fb_bram (13-bit: 1 buf + 12 addr)
    input wire  [31:0] fb_rd_data,    // 4 pixels per 32-bit word

    // Display buffer select
    input wire fb_display_buf_sel,    // Which half of BRAM to read

    // Palette write interface (from CPU clock domain)
    input wire        pal_wr,
    input wire [7:0]  pal_addr,
    input wire [23:0] pal_data,       // RGB888
    input wire        clk_pal_wr      // Clock for palette writes (CPU clock)
);

    // Video timing parameters
    localparam VID_V_BPORCH = 16;
    localparam VID_V_ACTIVE = 240;
    localparam VID_H_BPORCH = 40;
    localparam VID_H_ACTIVE = 320;

    localparam FB_WIDTH = 160;

    // Palette RAM: 256 entries x 24-bit RGB
    reg [23:0] palette [0:255];

    // Palette write (CPU clock domain)
    always @(posedge clk_pal_wr) begin
        if (pal_wr) begin
            palette[pal_addr] <= pal_data;
        end
    end

    // =========================================
    // Pixel address calculation with 2x upscale
    // =========================================
    //
    // Pipeline: addr (comb) -> BRAM read (1 clk) -> byte select + palette (comb) -> output (1 clk)
    // Total latency: 2 cycles. Pre-fetch address 2 pixels ahead to compensate.

    // Pre-fetch x by 2 pixels for pipeline compensation
    wire [9:0] pipe_x = x_count + 10'd2;

    // Source pixel coordinates (divide output by 2 for 2x upscale)
    wire [9:0] visible_x = pipe_x - VID_H_BPORCH;
    wire [9:0] visible_y = y_count - VID_V_BPORCH;
    wire [7:0] src_x = visible_x[8:1];    // 0..159
    wire [6:0] src_y = visible_y[7:1];    // 0..119

    // Pixel address = src_y * 160 + src_x
    // Decompose multiply: y*160 = y*128 + y*32 = (y<<7) + (y<<5)
    wire [14:0] pixel_addr = ({1'b0, src_y, 7'b0}) + ({3'b0, src_y, 5'b0}) + {7'b0, src_x};

    // 32-bit word address and byte select within word
    wire [12:0] word_addr = pixel_addr[14:2];
    wire [1:0]  byte_sel  = pixel_addr[1:0];

    // Add buffer base offset
    assign fb_rd_addr = {fb_display_buf_sel, word_addr[11:0]};

    // =========================================
    // Pixel read pipeline (2-stage: BRAM read -> output)
    // =========================================

    // Stage 1: Register byte_sel to sync with BRAM read data (1 cycle latency)
    reg [1:0] byte_sel_d1;
    always @(posedge clk_video) begin
        byte_sel_d1 <= byte_sel;
    end

    // Byte select from BRAM word (combinational, using delayed byte_sel)
    reg [7:0] palette_index;
    always @(*) begin
        case (byte_sel_d1)
            2'b00: palette_index = fb_rd_data[7:0];
            2'b01: palette_index = fb_rd_data[15:8];
            2'b10: palette_index = fb_rd_data[23:16];
            2'b11: palette_index = fb_rd_data[31:24];
        endcase
    end

    // Combinational palette lookup (12.288 MHz has ~81ns period, plenty of margin)
    wire [23:0] palette_rgb = palette[palette_index];

    // Active region detection (using pre-fetched x for pipeline alignment)
    wire in_hactive = (pipe_x >= VID_H_BPORCH) && (pipe_x < VID_H_BPORCH + VID_H_ACTIVE);
    wire in_vactive = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Delay active signal by 1 cycle to match BRAM read latency
    reg in_active_d1;
    always @(posedge clk_video) begin
        in_active_d1 <= in_hactive && in_vactive;
    end

    // Stage 2: Output pixel (registered)
    always @(posedge clk_video) begin
        if (in_active_d1)
            pixel_color <= palette_rgb;
        else
            pixel_color <= 24'h000000;
    end

endmodule
