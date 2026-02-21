//
// Video Scanout with SDRAM Framebuffer
// Reads RGB565 pixels from SDRAM using burst reads
// Uses a line buffer for clock domain crossing
//

`default_nettype none

module video_scanout (
    // Video clock domain (12.288 MHz)
    input wire clk_video,
    input wire reset_n,

    // Video timing inputs (active high)
    input wire [9:0] x_count,
    input wire [9:0] y_count,
    input wire line_start,          // Pulses at start of each line (x_count == 0)

    // Pixel output (RGB888)
    output reg [23:0] pixel_color,

    // Framebuffer base address (25-bit SDRAM word address)
    // For double buffering, this switches between front/back buffer addresses
    input wire [24:0] fb_base_addr,

    // SDRAM clock domain (133 MHz)
    input wire clk_sdram,

    // SDRAM burst interface
    output reg         burst_rd,
    output reg  [24:0] burst_addr,
    output reg  [10:0] burst_len,
    output wire        burst_32bit,
    input wire  [31:0] burst_data,
    input wire         burst_data_valid,
    input wire         burst_data_done
);

    // Video timing parameters
    localparam VID_V_BPORCH = 16;
    localparam VID_V_ACTIVE = 240;
    localparam VID_H_BPORCH = 40;
    localparam VID_H_ACTIVE = 320;

    // Line buffer: 320 pixels x 16 bits = 640 bytes
    // Dual-port RAM: write from SDRAM clock, read from video clock
    reg [15:0] line_buffer [0:319];
    reg [8:0] write_ptr;    // Write pointer (0-319)
    reg [8:0] read_ptr;     // Read pointer (0-319)

    // Use 32-bit burst mode (2 pixels per word)
    assign burst_32bit = 1'b1;

    // =========================================
    // Video clock domain - Line start detection
    // =========================================

    // Detect which line we need to fetch (next visible line)
    wire [9:0] fetch_line = y_count - VID_V_BPORCH + 1;  // Fetch line ahead
    wire in_vactive = (y_count >= VID_V_BPORCH - 1) && (y_count < VID_V_BPORCH + VID_V_ACTIVE - 1);

    // Generate fetch request at end of line (before next visible line)
    reg fetch_request;
    reg fetch_request_ack_sync1, fetch_request_ack_sync2;
    reg [8:0] fetch_line_latched;

    always @(posedge clk_video or negedge reset_n) begin
        if (!reset_n) begin
            fetch_request <= 0;
            fetch_line_latched <= 0;
            fetch_request_ack_sync1 <= 0;
            fetch_request_ack_sync2 <= 0;
        end else begin
            // Sync ack from SDRAM domain
            fetch_request_ack_sync1 <= fetch_request_ack;
            fetch_request_ack_sync2 <= fetch_request_ack_sync1;

            // Clear request when ack received
            if (fetch_request_ack_sync2)
                fetch_request <= 0;

            // Issue fetch request at line start if in active region
            if (line_start && in_vactive && !fetch_request) begin
                fetch_request <= 1;
                fetch_line_latched <= fetch_line[8:0];
            end
        end
    end

    // =========================================
    // Video clock domain - Pixel output
    // =========================================

    wire [9:0] visible_x = x_count - VID_H_BPORCH;
    wire in_hactive = (x_count >= VID_H_BPORCH) && (x_count < VID_H_BPORCH + VID_H_ACTIVE);
    wire in_vactive_display = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Read from line buffer and convert RGB565 to RGB888
    wire [15:0] pixel_rgb565 = line_buffer[visible_x[8:0]];
    wire [4:0] r5 = pixel_rgb565[15:11];
    wire [5:0] g6 = pixel_rgb565[10:5];
    wire [4:0] b5 = pixel_rgb565[4:0];

    always @(posedge clk_video) begin
        if (in_hactive && in_vactive_display) begin
            // RGB565 to RGB888: replicate MSBs into LSBs for proper scaling
            pixel_color <= {r5, r5[4:2], g6, g6[5:4], b5, b5[4:2]};
        end else begin
            pixel_color <= 24'h000000;
        end
    end

    // =========================================
    // SDRAM clock domain - Burst read FSM
    // =========================================

    // Sync fetch request to SDRAM domain
    reg fetch_request_sync1, fetch_request_sync2;
    reg fetch_request_ack;
    reg [8:0] fetch_line_sdram;

    // FSM states
    localparam ST_IDLE = 2'd0;
    localparam ST_BURST = 2'd1;
    localparam ST_WAIT = 2'd2;

    reg [1:0] state;

    always @(posedge clk_sdram or negedge reset_n) begin
        if (!reset_n) begin
            state <= ST_IDLE;
            burst_rd <= 0;
            burst_addr <= 0;
            burst_len <= 0;
            write_ptr <= 0;
            fetch_request_sync1 <= 0;
            fetch_request_sync2 <= 0;
            fetch_request_ack <= 0;
            fetch_line_sdram <= 0;
        end else begin
            // Sync fetch request
            fetch_request_sync1 <= fetch_request;
            fetch_request_sync2 <= fetch_request_sync1;

            // Default: deassert burst_rd
            burst_rd <= 0;

            case (state)
                ST_IDLE: begin
                    fetch_request_ack <= 0;

                    // Rising edge of fetch request
                    if (fetch_request_sync2 && !fetch_request_ack) begin
                        // Calculate SDRAM address for this line
                        // Each line is 320 pixels * 2 bytes = 640 bytes = 320 words (16-bit)
                        // burst_len is in 16-bit words (io_sdram counts 16-bit transfers)
                        fetch_line_sdram <= fetch_line_latched;
                        burst_addr <= fb_base_addr + {fetch_line_latched, 8'b0} + {1'b0, fetch_line_latched, 6'b0};
                        // burst_addr = base + line * 320 = base + line * 256 + line * 64
                        burst_len <= 11'd320;  // 320 x 16-bit words = 320 pixels
                        burst_rd <= 1;
                        write_ptr <= 0;
                        state <= ST_BURST;
                    end
                end

                ST_BURST: begin
                    // Write incoming data to line buffer
                    if (burst_data_valid) begin
                        // Each 32-bit word contains 2 RGB565 pixels (little-endian)
                        // burst_data[15:0] = first pixel (even address, lower addr)
                        // burst_data[31:16] = second pixel (odd address, higher addr)
                        line_buffer[write_ptr] <= burst_data[15:0];      // First pixel (even addr)
                        line_buffer[write_ptr + 1] <= burst_data[31:16]; // Second pixel (odd addr)
                        write_ptr <= write_ptr + 2;
                    end

                    if (burst_data_done) begin
                        fetch_request_ack <= 1;
                        state <= ST_WAIT;
                    end
                end

                ST_WAIT: begin
                    // Wait for fetch_request to clear before accepting new request
                    if (!fetch_request_sync2) begin
                        fetch_request_ack <= 0;
                        state <= ST_IDLE;
                    end
                end
            endcase
        end
    end

endmodule
