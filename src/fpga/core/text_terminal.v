//
// Text Terminal Display for 320x240 resolution
// 40 columns x 30 rows, 8x8 pixel font
// Memory-mapped at 0x20000000, 1200 bytes (40*30)
//

`default_nettype none

module text_terminal (
    input wire clk,           // Video clock (12.288 MHz)
    input wire clk_cpu,       // CPU clock (74.25 MHz)
    input wire reset_n,

    // Video interface
    input wire [9:0] pixel_x,
    input wire [9:0] pixel_y,
    output reg [23:0] pixel_color,

    // CPU memory interface (directly exposed for memory mapping)
    input wire        mem_valid,
    input wire [31:0] mem_addr,
    input wire [31:0] mem_wdata,
    input wire [3:0]  mem_wstrb,
    output reg [31:0] mem_rdata,
    output reg        mem_ready
);

// Terminal dimensions (320x240 with 8x8 font = 40x30 characters)
localparam TERM_COLS = 40;
localparam TERM_ROWS = 30;
localparam TERM_SIZE = TERM_COLS * TERM_ROWS;  // 1200 characters

// Character dimensions
localparam CHAR_WIDTH = 8;
localparam CHAR_HEIGHT = 8;

// No prefetch offset needed - pipeline delay handled by delayed pixel_col
wire [9:0] fetch_x = pixel_x;

// Calculate character position from pixel coordinates (using pre-fetch X)
wire [5:0] char_col = fetch_x[8:3];  // fetch_x / 8 (max 39 for 40 cols)
wire [4:0] char_row = pixel_y[7:3];  // pixel_y / 8 (max 29 for 30 rows)
wire [2:0] pixel_col = fetch_x[2:0]; // fetch_x % 8
wire [2:0] pixel_row = pixel_y[2:0]; // pixel_y % 8

// Calculate VRAM address for current character
// char_index = char_row * 40 + char_col
// Using shifts: char_row * 40 = char_row * 32 + char_row * 8 = (char_row << 5) + (char_row << 3)
wire [10:0] char_index = ({char_row, 5'b0} + {char_row, 3'b0} + char_col);
wire [8:0] vram_word_addr = char_index[10:2];  // Divide by 4 for 32-bit words
wire [1:0]  vram_byte_sel = char_index[1:0];    // Which byte in the word

// CPU address decoding
wire cpu_addr_valid = (mem_addr[31:13] == 19'h10000);  // 0x20000000 range
wire [10:0] cpu_word_addr = mem_addr[12:2];

// VRAM using dual-port block RAM
// Port A: Video read (continuous)
// Port B: CPU read/write
wire [31:0] vram_video_data;
wire [31:0] vram_cpu_data;

altsyncram #(
    .operation_mode("BIDIR_DUAL_PORT"),
    .width_a(32),
    .widthad_a(9),
    .numwords_a(300),   // 40*30/4 = 300 words
    .width_b(32),
    .widthad_b(9),
    .numwords_b(300),
    .width_byteena_b(4),
    .lpm_type("altsyncram"),
    .outdata_reg_a("UNREGISTERED"),
    .outdata_reg_b("UNREGISTERED"),
    .init_file("core/vram_init.mif"),
    .intended_device_family("Cyclone V"),
    .read_during_write_mode_port_a("NEW_DATA_NO_NBE_READ"),
    .read_during_write_mode_port_b("NEW_DATA_NO_NBE_READ")
) vram (
    // Port A - Video (read only)
    .clock0(clk),
    .address_a(vram_word_addr),
    .data_a(32'b0),
    .wren_a(1'b0),
    .q_a(vram_video_data),

    // Port B - CPU (read/write) - uses CPU clock for proper CDC
    .clock1(clk_cpu),
    .address_b(cpu_word_addr),
    .data_b(mem_wdata),
    .wren_b(mem_valid && !mem_ready && cpu_addr_valid && |mem_wstrb),
    .byteena_b(mem_wstrb),
    .q_b(vram_cpu_data),

    // Unused ports
    .aclr0(1'b0),
    .aclr1(1'b0),
    .addressstall_a(1'b0),
    .addressstall_b(1'b0),
    .byteena_a(1'b1),
    .clocken0(1'b1),
    .clocken1(1'b1),
    .clocken2(1'b1),
    .clocken3(1'b1),
    .eccstatus(),
    .rden_a(1'b1),
    .rden_b(1'b1)
);

// Pipeline stage 1: VRAM read latency
reg [1:0] vram_byte_sel_d1;
reg [2:0] pixel_col_d1;
reg [2:0] pixel_row_d1;
reg [9:0] pixel_x_d1;
reg [9:0] pixel_y_d1;

always @(posedge clk) begin
    vram_byte_sel_d1 <= vram_byte_sel;
    pixel_col_d1 <= pixel_col;
    pixel_row_d1 <= pixel_row;
    pixel_x_d1 <= pixel_x;
    pixel_y_d1 <= pixel_y;
end

// Select character byte from VRAM word
reg [7:0] current_char;
always @(*) begin
    case (vram_byte_sel_d1)
        2'd0: current_char = vram_video_data[7:0];
        2'd1: current_char = vram_video_data[15:8];
        2'd2: current_char = vram_video_data[23:16];
        2'd3: current_char = vram_video_data[31:24];
    endcase
end

// Font ROM using block RAM with MIF initialization
// 768 bytes: 96 characters * 8 rows
wire [9:0] font_addr;
wire [7:0] font_data;

// Get font data for current character
// Font ROM index = (char - 32) * 8 + pixel_row
wire [7:0] char_offset = (current_char >= 8'd32 && current_char < 8'd128) ?
                         (current_char - 8'd32) : 8'd0;
assign font_addr = {char_offset[6:0], pixel_row_d1};

altsyncram #(
    .operation_mode("ROM"),
    .width_a(8),
    .widthad_a(10),
    .numwords_a(768),
    .lpm_type("altsyncram"),
    .outdata_reg_a("UNREGISTERED"),
    .init_file("core/font_rom.mif"),
    .intended_device_family("Cyclone V")
) font_rom (
    .clock0(clk),
    .address_a(font_addr),
    .q_a(font_data),
    // Unused ports
    .aclr0(1'b0),
    .aclr1(1'b0),
    .address_b(1'b0),
    .addressstall_a(1'b0),
    .addressstall_b(1'b0),
    .byteena_a(1'b1),
    .byteena_b(1'b1),
    .clock1(1'b1),
    .clocken0(1'b1),
    .clocken1(1'b1),
    .clocken2(1'b1),
    .clocken3(1'b1),
    .data_a({8{1'b0}}),
    .data_b({8{1'b0}}),
    .eccstatus(),
    .q_b(),
    .rden_a(1'b1),
    .rden_b(1'b0),
    .wren_a(1'b0),
    .wren_b(1'b0)
);

// Pipeline stage 2: Font ROM read latency
reg [2:0] pixel_col_d2;
reg [9:0] pixel_x_d2;
reg [9:0] pixel_y_d2;

always @(posedge clk) begin
    pixel_col_d2 <= pixel_col_d1;
    pixel_x_d2 <= pixel_x_d1;
    pixel_y_d2 <= pixel_y_d1;
end

// Visible area check uses the delayed (actual display) coordinates
wire in_visible_area = (pixel_x_d2 < 320) && (pixel_y_d2 < 240);

// Get pixel value (MSB first) - use delayed pixel_col due to ROM latency
wire pixel_on = font_data[7 - pixel_col_d2];

// Generate pixel color
always @(*) begin
    if (in_visible_area && pixel_on)
        pixel_color = 24'hFFFFFF;  // White text
    else if (in_visible_area)
        pixel_color = 24'h000020;  // Dark blue background
    else
        pixel_color = 24'h000000;  // Black outside visible area
end

// Memory interface for CPU - handle ready signal and read data
// BRAM has 1 cycle read latency, so we need to delay ready
// Uses clk_cpu to match CPU clock domain
reg mem_pending;

always @(posedge clk_cpu) begin
    mem_ready <= 0;

    if (!reset_n) begin
        mem_pending <= 0;
    end else if (mem_valid && !mem_ready && !mem_pending) begin
        // Start of access - data will be available next cycle
        mem_pending <= 1;
    end else if (mem_pending) begin
        // Data is now available from BRAM
        mem_ready <= 1;
        mem_pending <= 0;

        if (cpu_addr_valid)
            mem_rdata <= vram_cpu_data;
        else
            mem_rdata <= 32'h0;
    end
end

endmodule
