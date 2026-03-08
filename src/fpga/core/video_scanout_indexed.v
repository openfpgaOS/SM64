//
// Video Scanout with 8-bit Indexed Color and Hardware Palette
// Reads 8-bit palette indices from SDRAM, looks up RGB565 in palette RAM
//

`default_nettype none

module video_scanout_indexed (
    // Video clock domain (12.288 MHz)
    input wire clk_video,
    input wire reset_n,

    // Video timing inputs (active high)
    input wire [9:0] x_count,
    input wire [9:0] y_count,
    input wire line_start,          // Pulses at start of each line (x_count == 0)

    // Pixel output (RGB888)
    output reg [23:0] pixel_color,

    // Framebuffer base address (25-bit SDRAM byte address >> 1 = 16-bit word address)
    input wire [24:0] fb_base_addr,

    // SDRAM clock domain (66 MHz)
    input wire clk_sdram,

    // SDRAM burst interface
    output reg         burst_rd,
    output reg  [24:0] burst_addr,
    output reg  [10:0] burst_len,
    output wire        burst_32bit,
    input wire  [31:0] burst_data,
    input wire         burst_data_valid,
    input wire         burst_data_done,

    // Palette write interface (directly from CPU, active on any clock edge)
    input wire        pal_wr,
    input wire [7:0]  pal_addr,
    input wire [23:0] pal_data      // RGB888 from Quake palette
);

    // Video timing parameters
    localparam VID_V_BPORCH = 16;
    localparam VID_V_ACTIVE = 240;
    localparam VID_H_BPORCH = 40;
    localparam VID_H_ACTIVE = 320;

    // Line buffer: 320 x 8-bit palette indices (native 320x240, no upscale)
    // Store as 16-bit words for efficient SDRAM reads
    reg [15:0] line_buffer [0:159];  // 160 x 16-bit = 320 x 8-bit indices
    reg [7:0] write_ptr;    // Write pointer (0-159 for 16-bit words)

    // Palette RAM: 256 entries x 24-bit RGB
    // Using inferred dual-port RAM
    reg [23:0] palette [0:255];

    // Use 32-bit burst mode (4 pixels per 32-bit word)
    assign burst_32bit = 1'b1;

    // =========================================
    // Palette write (directly driven, no clock)
    // =========================================
    always @(posedge clk_sdram) begin
        if (pal_wr) begin
            palette[pal_addr] <= pal_data;
        end
    end

    // =========================================
    // Video clock domain - Line start detection
    // =========================================

    // Detect which source line to fetch for the current display line.
    // No fetch-ahead: the burst completes (~0.6µs) well before active
    // pixels start (H back porch = 40 px ≈ 3.25µs), so the line buffer
    // is ready in time.  This avoids the single-buffer overwrite hazard
    // that occurs with 2x vertical upscale (two display lines share one
    // source line; fetching ahead would clobber the buffer mid-pair).
    wire [9:0] fetch_line = y_count - VID_V_BPORCH;
    wire in_vactive = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Generate fetch request at end of line (before next visible line)
    reg fetch_request;
    reg fetch_request_ack_sync1, fetch_request_ack_sync2;
    reg [8:0] fetch_line_latched;
    reg [8:0] last_fetched_line;      // Track last fetched source line
    reg       last_fetched_valid;     // Whether last_fetched_line is valid

    always @(posedge clk_video or negedge reset_n) begin
        if (!reset_n) begin
            fetch_request <= 0;
            fetch_line_latched <= 0;
            fetch_request_ack_sync1 <= 0;
            fetch_request_ack_sync2 <= 0;
            last_fetched_line <= 9'd511;  // Invalid value to force first fetch
            last_fetched_valid <= 0;
        end else begin
            // Sync ack from SDRAM domain
            fetch_request_ack_sync1 <= fetch_request_ack;
            fetch_request_ack_sync2 <= fetch_request_ack_sync1;

            // Clear request when ack received; mark line as fetched
            if (fetch_request_ack_sync2) begin
                fetch_request <= 0;
                last_fetched_line <= fetch_line_latched;
                last_fetched_valid <= 1;
            end

            // Issue fetch request at line start if in active region
            // Native 320x240: no vertical upscale, each display line = unique source line
            // Skip fetch if same source line already in the line buffer
            if (line_start && in_vactive && !fetch_request) begin
                if (!last_fetched_valid || (fetch_line[8:0] != last_fetched_line)) begin
                    fetch_request <= 1;
                    fetch_line_latched <= fetch_line[8:0];
                end
            end

            // Invalidate at start of VBlank so first line of next frame always fetches
            if (line_start && !in_vactive)
                last_fetched_valid <= 0;
        end
    end

    // =========================================
    // Video clock domain - Pixel output
    // =========================================

    wire [9:0] visible_x = x_count - VID_H_BPORCH;
    wire in_hactive = (x_count >= VID_H_BPORCH) && (x_count < VID_H_BPORCH + VID_H_ACTIVE);
    wire in_vactive_display = (y_count >= VID_V_BPORCH) && (y_count < VID_V_BPORCH + VID_V_ACTIVE);

    // Read 8-bit index from line buffer (native 320x240, no horizontal upscale)
    // visible_x[8:1] selects 16-bit word (0-159), visible_x[0] selects byte within word
    wire [7:0] word_idx = visible_x[8:1];
    wire [15:0] pixel_word = line_buffer[word_idx];
    wire [7:0] palette_index = visible_x[0] ? pixel_word[15:8] : pixel_word[7:0];

    // Lookup palette (registered for timing)
    reg [23:0] palette_rgb;
    always @(posedge clk_video) begin
        palette_rgb <= palette[palette_index];
    end

    always @(posedge clk_video) begin
        if (in_hactive && in_vactive_display) begin
            pixel_color <= palette_rgb;
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
                        // Calculate SDRAM address for this source line
                        // Each source line is 320 bytes = 160 x 16-bit words
                        // line_addr = base + source_line * 160
                        fetch_line_sdram <= fetch_line_latched;
                        // burst_addr = base + line * 160 = base + line * 128 + line * 32
                        burst_addr <= fb_base_addr + {fetch_line_latched, 7'b0} + {2'b0, fetch_line_latched, 5'b0};
                        burst_len <= 11'd80;   // 80 READ cmds x BL=2 = 160 x 16-bit words = 320 pixels
                        burst_rd <= 1;
                        write_ptr <= 0;
                        state <= ST_BURST;
                    end
                end

                ST_BURST: begin
                    // Write incoming data to line buffer
                    if (burst_data_valid) begin
                        // Each 32-bit word contains 4 palette indices (4 x 8-bit)
                        // Store as two 16-bit entries in line buffer
                        line_buffer[write_ptr] <= burst_data[15:0];      // First 2 pixels
                        line_buffer[write_ptr + 1] <= burst_data[31:16]; // Next 2 pixels
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
