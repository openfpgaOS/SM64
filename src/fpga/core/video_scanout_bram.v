//
// Video Scanout from BRAM Framebuffer
//
// Reads RGB332 pixels from a dual-port BRAM framebuffer (160x120),
// looks up RGB888 via a 256-entry palette, and outputs with 2x upscale
// (320x240 display from 160x120 source).
//
// Port B of the FB BRAM runs at clk_video.  The BRAM is dual-clock
// (port A at clk_cpu, port B at clk_video), handled by altsyncram.
//

`default_nettype none

module video_scanout_bram (
    // Video clock domain (12.288 MHz)
    input wire clk_video,
    input wire reset_n,

    // Video timing inputs
    input wire [9:0] x_count,
    input wire [9:0] y_count,

    // Pixel output (RGB888)
    output reg [23:0] pixel_color,

    // FB BRAM port B (read-only, clocked at clk_video)
    output wire [13:0] fb_rd_addr,    // Word address (9600 words, double-buffered)
    input  wire [31:0] fb_rd_data,    // 4 x RGB332 pixels per word

    // Double-buffer page select (display page = opposite of draw page)
    input wire         fb_display_page,

    // Palette write interface (directly from CPU via clk_cpu)
    input wire        pal_wr,
    input wire [7:0]  pal_addr,
    input wire [23:0] pal_data,
    input wire        pal_clk
);

    // Video timing parameters
    localparam VID_V_BPORCH = 16;
    localparam VID_V_ACTIVE = 240;
    localparam VID_H_BPORCH = 40;
    localparam VID_H_ACTIVE = 320;

    // Source resolution
    localparam SRC_W = 160;
    localparam SRC_H = 120;

    // Palette RAM: 256 entries x 24-bit RGB (dual-clock: write at pal_clk, read at clk_video)
    reg [23:0] palette [0:255];

    always @(posedge pal_clk) begin
        if (pal_wr)
            palette[pal_addr] <= pal_data;
    end

    // =========================================
    // Pixel address calculation with 2x upscale
    // =========================================

    wire in_hactive = (x_count >= VID_H_BPORCH) && (x_count < VID_H_BPORCH + VID_H_ACTIVE);
    wire in_vactive = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Source pixel coordinates (2x downscale from display coordinates)
    wire [9:0] visible_x = x_count - VID_H_BPORCH;
    wire [9:0] visible_y = y_count - VID_V_BPORCH;
    wire [7:0] src_x = visible_x[8:1];   // 0..159 (div by 2)
    wire [7:0] src_y = visible_y[8:1];   // 0..119 (div by 2)

    // Pixel index = src_y * 160 + src_x
    // 160 = 128 + 32, so y*160 = y*128 + y*32 = (y<<7) + (y<<5)
    wire [14:0] pixel_idx = ({7'b0, src_y} << 7) + ({7'b0, src_y} << 5) + {7'b0, src_x};

    // BRAM word address (4 pixels per 32-bit word) + display page offset
    wire [12:0] fb_rd_addr_page = pixel_idx[14:2];
    assign fb_rd_addr = fb_display_page ? ({1'b0, fb_rd_addr_page} + 14'd4800) : {1'b0, fb_rd_addr_page};

    // Select byte within word (registered for timing — 1 cycle BRAM latency)
    reg [1:0] byte_sel_r;
    always @(posedge clk_video) begin
        byte_sel_r <= pixel_idx[1:0];
    end

    // Extract RGB332 pixel from BRAM word
    reg [7:0] rgb332;
    always @(*) begin
        case (byte_sel_r)
            2'd0: rgb332 = fb_rd_data[7:0];
            2'd1: rgb332 = fb_rd_data[15:8];
            2'd2: rgb332 = fb_rd_data[23:16];
            2'd3: rgb332 = fb_rd_data[31:24];
        endcase
    end

    // Palette lookup (registered)
    reg [23:0] palette_rgb;
    always @(posedge clk_video) begin
        palette_rgb <= palette[rgb332];
    end

    // =========================================
    // Pixel output (2 cycles behind address: BRAM + palette)
    // =========================================

    // Delay active signals to match pipeline (2 cycles)
    reg in_active_d1, in_active_d2;
    always @(posedge clk_video) begin
        in_active_d1 <= in_hactive && in_vactive;
        in_active_d2 <= in_active_d1;
    end

    always @(posedge clk_video) begin
        if (in_active_d2)
            pixel_color <= palette_rgb;
        else
            pixel_color <= 24'h000000;
    end

endmodule
