//
// Span Rasterizer — Hardware pixel pipeline for SM64
//
// CPU does triangle setup + edge walking, pushes per-scanline span commands
// into a FIFO.  Hardware pops commands and processes pixels asynchronously,
// overlapping rasterization with CPU display list processing.
//
// Two command types:
//   CONFIG   (tag=01, 14 words): per-triangle setup (mode, dp values, tex config)
//   SCANLINE (tag=10, 10 words): per-scanline span (fb_idx, count, property start values)
//
// Z-buffer and framebuffer live in shared dual-port BRAMs (instantiated
// in core_top.v).  Port A = CPU, Port B = this rasterizer.
//
// FB format: RGB332 (8-bit), packed 4 pixels per 32-bit word.
// ZB format: 16-bit, packed 2 values per 32-bit word.
//
// 3 modes: 0=texrgb (tex*vertex color), 1=tex only, 2=rgb (vertex color only)
//

`default_nettype none

module span_rasterizer (
    input wire clk,
    input wire reset_n,

    // Register + texture BRAM write interface (from axi_periph_slave)
    input wire         reg_wr,
    input wire [12:0]  reg_addr,   // [12]: 0=registers/FIFO, 1=texture BRAM
    input wire [31:0]  reg_wdata,
    output reg [31:0]  reg_rdata,

    // Z-buffer BRAM port B (19200 x 16-bit as 9600 x 32-bit)
    output reg  [13:0] zb_bram_addr,
    input  wire [31:0] zb_bram_rdata,
    output reg  [31:0] zb_bram_wdata,
    output reg  [3:0]  zb_bram_wstrb,

    // Framebuffer BRAM port B (19200 x 8-bit as 4800 x 32-bit)
    output reg  [12:0] fb_bram_addr,
    input  wire [31:0] fb_bram_rdata,
    output reg  [31:0] fb_bram_wdata,
    output reg  [3:0]  fb_bram_wstrb,

    output wire        active
);

// ============================================================
// Command FIFO (1024 x 32-bit = 4KB)
// ============================================================
reg [31:0] cmd_fifo [0:1023];
reg [9:0]  fifo_wr_ptr;
reg [9:0]  fifo_rd_ptr;

wire [9:0] fifo_count = fifo_wr_ptr - fifo_rd_ptr;
wire       fifo_empty = (fifo_wr_ptr == fifo_rd_ptr);
wire       fifo_full  = (fifo_count == 10'd1023);
wire [9:0] fifo_free  = 10'd1023 - fifo_count;

// FIFO read port (1-cycle latency)
reg [31:0] fifo_rd_data;
always @(posedge clk) fifo_rd_data <= cmd_fifo[fifo_rd_ptr];

// FIFO write from CPU
wire fifo_push = reg_wr && ~reg_addr[12] && (reg_addr[7:2] == 6'd32); // addr offset 0x80
always @(posedge clk) begin
    if (fifo_push && !fifo_full)
        cmd_fifo[fifo_wr_ptr] <= reg_wdata;
end
always @(posedge clk or negedge reset_n) begin
    if (!reset_n)
        fifo_wr_ptr <= 0;
    else if (fifo_push && !fifo_full)
        fifo_wr_ptr <= fifo_wr_ptr + 1;
end

// Command tags (bits [31:30] of first word)
localparam TAG_CONFIG   = 2'b01;
localparam TAG_SCANLINE = 2'b10;

// ============================================================
// Configuration Registers (loaded from FIFO CONFIG commands)
// ============================================================

// Per-triangle setup
reg [1:0]  cfg_mode;           // 0=texrgb, 1=tex, 2=rgb
reg        cfg_z_test;
reg        cfg_z_write;
reg signed [31:0] cfg_dp_z, cfg_dp_w;
reg signed [31:0] cfg_dp_u, cfg_dp_v;
reg signed [31:0] cfg_dp_r, cfg_dp_g, cfg_dp_b;
reg signed [31:0] cfg_tex_fw, cfg_tex_fh;   // Texture dimensions, Q16.16
reg [8:0]  cfg_tex_wrap_w, cfg_tex_wrap_h;
reg [3:0]  cfg_tex_log2_w;
reg signed [31:0] cfg_z_offset;

// Per-span
reg [14:0] cfg_fb_idx;
reg [14:0] cfg_zb_idx;
reg [8:0]  cfg_count;
reg signed [31:0] cfg_p_z, cfg_p_w;
reg signed [31:0] cfg_p_u, cfg_p_v;
reg signed [31:0] cfg_p_r, cfg_p_g, cfg_p_b;

reg        busy;
reg [31:0] ztest_pass;
reg [31:0] ztest_reject;

assign active = busy;

// ============================================================
// Register read (status + FIFO info)
// ============================================================
wire reg_is_tex = reg_addr[12];
wire [5:0] reg_idx = reg_addr[7:2];

always @(*) begin
    if (!reg_is_tex && reg_idx == 6'd27)
        reg_rdata = {31'b0, busy};
    else if (!reg_is_tex && reg_idx == 6'd33)  // 0x84: FIFO status
        reg_rdata = {6'b0, fifo_free, 6'b0, fifo_count};
    else if (!reg_is_tex && reg_idx == 6'd34)  // 0x88: z-test pass count
        reg_rdata = ztest_pass;
    else if (!reg_is_tex && reg_idx == 6'd35)  // 0x8C: z-test reject count
        reg_rdata = ztest_reject;
    else
        reg_rdata = 32'h0;
end

// ============================================================
// Texture BRAM (4KB = 1024 x 32-bit RGBA texels)
// ============================================================
reg [31:0] tex_bram [0:1023];
reg [9:0]  tex_rd_addr;
reg [31:0] tex_rd_data;

always @(posedge clk) begin
    if (reg_wr && reg_is_tex)
        tex_bram[reg_addr[11:2]] <= reg_wdata;
    tex_rd_data <= tex_bram[tex_rd_addr];
end

// ============================================================
// Reciprocal LUT (2048 x 16-bit as 1024 x 32-bit)
// ============================================================
reg [31:0] rcp_lut [0:1023];
reg [9:0]  rcp_rd_addr;
reg [31:0] rcp_rd_data;

always @(posedge clk) rcp_rd_data <= rcp_lut[rcp_rd_addr];

integer k;
initial begin
    for (k = 0; k < 1024; k = k + 1) begin : blk_rcp_init
        integer even_val, odd_val;
        even_val = 65536 * 2048 / (2048 + 2*k);
        odd_val  = 65536 * 2048 / (2048 + 2*k + 1);
        if (even_val > 65535) even_val = 65535;
        rcp_lut[k] = (odd_val << 16) | even_val;
    end
end

// ============================================================
// Multiplier (32x32 → 64 signed, registered)
// Set inputs cycle T → result available cycle T+2
// ============================================================
reg signed [31:0] mul_a, mul_b;
reg signed [63:0] mul_result;
always @(posedge clk) mul_result <= mul_a * mul_b;
wire signed [31:0] mul_q16 = mul_result[47:16];

// ============================================================
// CLZ
// ============================================================
function [4:0] clz32;
    input [31:0] val;
    integer n;
    begin
        clz32 = 5'd31;
        for (n = 31; n >= 0; n = n - 1)
            if (val[n]) clz32 = 31 - n;
    end
endfunction

// ============================================================
// Working registers
// ============================================================
reg signed [31:0] cur_z, cur_w, cur_u, cur_v, cur_r, cur_g, cur_b;
reg [8:0]  px_idx;            // Pixel counter within span
reg [14:0] cur_fb_idx;        // Current absolute pixel index in FB
reg [14:0] cur_zb_idx;        // Current absolute pixel index in ZB
reg [15:0] cur_zbuf;          // Current pixel z-value
reg [31:0] zb_word_saved;     // Saved ZB word for read-modify-write
reg signed [31:0] rcp_w;
reg signed [31:0] u_persp, v_persp;
reg [7:0]  r_byte, g_byte, b_byte;
reg [31:0] tex_color;
reg [7:0]  pixel_out_332;     // RGB332 output pixel
reg [8:0]  tx_saved;

// RCP working regs
reg [4:0]  rcp_lz;
reg        rcp_mantissa_lsb;
reg        rcp_w_sign;

// ============================================================
// Combinational RCP signals
// ============================================================
wire [31:0] w_abs = cur_w[31] ? (~cur_w + 1) : cur_w;
wire [4:0]  w_clz = clz32(w_abs);
wire [31:0] w_shifted = w_abs << (w_clz + 1);
wire [10:0] w_mantissa = w_shifted[31:21];

wire [15:0] rcp_lut_val = rcp_mantissa_lsb ? rcp_rd_data[31:16] : rcp_rd_data[15:0];
wire [31:0] rcp_denorm = (rcp_lz >= 5'd15)
    ? ({16'b0, rcp_lut_val} << (rcp_lz - 5'd15))
    : ({16'b0, rcp_lut_val} >> (5'd15 - rcp_lz));
wire signed [31:0] rcp_final = rcp_w_sign ? (~rcp_denorm + 1) : rcp_denorm;

// ============================================================
// FSM — FIFO dispatch + pixel pipeline
// ============================================================
localparam [4:0]
    ST_FIFO_IDLE     = 5'd0,
    ST_PX_ZSETUP     = 5'd1,
    ST_PX_ZWAIT      = 5'd2,
    ST_PX_ZTEST      = 5'd3,
    ST_MUL_WAIT      = 5'd4,
    ST_PX_U_DONE     = 5'd5,
    ST_PX_V_DONE     = 5'd6,
    ST_PX_TU_DONE    = 5'd7,
    ST_PX_TV_DONE    = 5'd8,
    ST_PX_TEXR       = 5'd9,
    ST_PX_G_DONE     = 5'd10,
    ST_PX_B_DONE     = 5'd11,
    ST_PX_WR         = 5'd12,
    ST_PX_NEXT       = 5'd13,
    ST_FIFO_READ_TAG = 5'd14,
    ST_FIFO_CONFIG   = 5'd15,
    ST_FIFO_SCANLINE = 5'd16;

reg [4:0] state;
reg [4:0] mul_return;
reg [3:0] fifo_word_idx;   // Word counter for multi-word FIFO reads

always @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
        state <= ST_FIFO_IDLE;
        busy <= 0;
        ztest_pass <= 0;
        ztest_reject <= 0;
        fifo_rd_ptr <= 0;
        fifo_word_idx <= 0;
        zb_bram_wstrb <= 4'b0;
        fb_bram_wstrb <= 4'b0;
        zb_bram_addr <= 0; zb_bram_wdata <= 0;
        fb_bram_addr <= 0; fb_bram_wdata <= 0;
        px_idx <= 0;
        cur_fb_idx <= 0; cur_zb_idx <= 0;
        cur_z <= 0; cur_w <= 0; cur_u <= 0; cur_v <= 0;
        cur_r <= 0; cur_g <= 0; cur_b <= 0;
        cur_zbuf <= 0; zb_word_saved <= 0;
        rcp_w <= 0; rcp_lz <= 0; rcp_mantissa_lsb <= 0; rcp_w_sign <= 0;
        u_persp <= 0; v_persp <= 0;
        r_byte <= 0; g_byte <= 0; b_byte <= 0;
        tex_color <= 0; pixel_out_332 <= 0;
        mul_a <= 0; mul_b <= 0; mul_return <= 0;
        tex_rd_addr <= 0; rcp_rd_addr <= 0;
        tx_saved <= 0;
        cfg_fb_idx <= 0; cfg_zb_idx <= 0; cfg_count <= 0;
        cfg_p_z <= 0; cfg_p_w <= 0;
        cfg_p_u <= 0; cfg_p_v <= 0;
        cfg_p_r <= 0; cfg_p_g <= 0; cfg_p_b <= 0;
        cfg_dp_z <= 0; cfg_dp_w <= 0;
        cfg_dp_u <= 0; cfg_dp_v <= 0;
        cfg_dp_r <= 0; cfg_dp_g <= 0; cfg_dp_b <= 0;
        cfg_tex_fw <= 0; cfg_tex_fh <= 0;
        cfg_tex_wrap_w <= 0; cfg_tex_wrap_h <= 0;
        cfg_tex_log2_w <= 0; cfg_z_offset <= 0;
        cfg_mode <= 0; cfg_z_test <= 1; cfg_z_write <= 1;
    end else begin

        // Default: deassert write strobes
        zb_bram_wstrb <= 4'b0;
        fb_bram_wstrb <= 4'b0;

        case (state)

        // ================================================================
        // FIFO IDLE — check for commands
        // ================================================================
        ST_FIFO_IDLE: begin
            if (!fifo_empty) begin
                busy <= 1;
                // fifo_rd_data will be available next cycle (BRAM read latency)
                state <= ST_FIFO_READ_TAG;
            end else begin
                busy <= 0;
            end
        end

        // ================================================================
        // Read tag word from FIFO and dispatch
        // ================================================================
        ST_FIFO_READ_TAG: begin
            fifo_rd_ptr <= fifo_rd_ptr + 1;  // consume tag word
            case (fifo_rd_data[31:30])
                TAG_CONFIG: begin
                    // Word 0 also contains mode/flags in lower bits
                    {cfg_z_write, cfg_z_test, cfg_mode} <= fifo_rd_data[3:0];
                    fifo_word_idx <= 0;
                    state <= ST_FIFO_CONFIG;
                end
                TAG_SCANLINE: begin
                    // Word 0 lower bits contain count
                    cfg_count <= fifo_rd_data[8:0];
                    fifo_word_idx <= 0;
                    state <= ST_FIFO_SCANLINE;
                end
                default: begin
                    // Unknown tag, skip and return to idle
                    state <= ST_FIFO_IDLE;
                end
            endcase
        end

        // ================================================================
        // Read CONFIG command (13 remaining words after tag)
        // word_idx 0 = BRAM settle cycle (fifo_rd_data still has tag)
        // word_idx 1..13 = read data words
        // ================================================================
        ST_FIFO_CONFIG: begin
            fifo_word_idx <= fifo_word_idx + 1;
            if (fifo_word_idx < 4'd13)
                fifo_rd_ptr <= fifo_rd_ptr + 1;  // advance for next read
            case (fifo_word_idx)
                4'd0:  ;  // BRAM settling — fifo_rd_data not valid yet
                4'd1:  cfg_dp_z <= fifo_rd_data;
                4'd2:  cfg_dp_w <= fifo_rd_data;
                4'd3:  cfg_dp_u <= fifo_rd_data;
                4'd4:  cfg_dp_v <= fifo_rd_data;
                4'd5:  cfg_dp_r <= fifo_rd_data;
                4'd6:  cfg_dp_g <= fifo_rd_data;
                4'd7:  cfg_dp_b <= fifo_rd_data;
                4'd8:  cfg_tex_fw <= fifo_rd_data;
                4'd9:  cfg_tex_fh <= fifo_rd_data;
                4'd10: cfg_tex_wrap_w <= fifo_rd_data[8:0];
                4'd11: cfg_tex_wrap_h <= fifo_rd_data[8:0];
                4'd12: cfg_tex_log2_w <= fifo_rd_data[3:0];
                4'd13: begin
                    cfg_z_offset <= fifo_rd_data;
                    state <= ST_FIFO_IDLE;  // CONFIG done
                end
                default: state <= ST_FIFO_IDLE;
            endcase
        end

        // ================================================================
        // Read SCANLINE command (9 remaining words after tag)
        // word_idx 0 = BRAM settle cycle
        // word_idx 1..9 = read data words
        // ================================================================
        ST_FIFO_SCANLINE: begin
            fifo_word_idx <= fifo_word_idx + 1;
            if (fifo_word_idx < 4'd9)
                fifo_rd_ptr <= fifo_rd_ptr + 1;  // advance for next read
            case (fifo_word_idx)
                4'd0:  ;  // BRAM settling — fifo_rd_data not valid yet
                4'd1:  cfg_fb_idx <= fifo_rd_data[14:0];
                4'd2:  cfg_zb_idx <= fifo_rd_data[14:0];
                4'd3:  cfg_p_z <= fifo_rd_data;
                4'd4:  cfg_p_w <= fifo_rd_data;
                4'd5:  cfg_p_u <= fifo_rd_data;
                4'd6:  cfg_p_v <= fifo_rd_data;
                4'd7:  cfg_p_r <= fifo_rd_data;
                4'd8:  cfg_p_g <= fifo_rd_data;
                4'd9: begin
                    cfg_p_b <= fifo_rd_data;
                    // Start pixel processing — use registered cfg values
                    cur_z <= cfg_p_z; cur_w <= cfg_p_w;
                    cur_u <= cfg_p_u; cur_v <= cfg_p_v;
                    cur_r <= cfg_p_r; cur_g <= cfg_p_g;
                    cur_b <= fifo_rd_data;  // p_b not yet registered
                    px_idx <= 0;
                    cur_fb_idx <= cfg_fb_idx;
                    cur_zb_idx <= cfg_zb_idx;
                    state <= ST_PX_ZSETUP;
                end
                default: state <= ST_FIFO_IDLE;
            endcase
        end

        // ================================================================
        // Pixel Processing — direct BRAM access (unchanged from original)
        // ================================================================

        ST_PX_ZSETUP: begin
            if (px_idx >= cfg_count) begin
                state <= ST_FIFO_IDLE;  // Span done, check FIFO for next command
            end else begin
                // Issue ZB BRAM read (word address = zb_idx / 2)
                zb_bram_addr <= cur_zb_idx[14:1];
                // Issue RCP LUT read (speculative)
                rcp_rd_addr <= w_mantissa[10:1];
                rcp_lz <= w_clz;
                rcp_mantissa_lsb <= w_mantissa[0];
                rcp_w_sign <= cur_w[31];
                state <= ST_PX_ZWAIT;
            end
        end

        ST_PX_ZWAIT: begin
            // Compute z-value while BRAMs settle (1 cycle latency)
            // Exact match for SW: RV_Z_TO_ZBUF(v) = ((int64_t)(v) * 65535) >> 16
            // = (v * (2^16 - 1)) >> 16 = v - 1 when v[15:0] != 0, else v
            begin : blk_zcompute
                reg signed [31:0] z_raw;
                z_raw = cur_z - (|cur_z[15:0]) + cfg_z_offset;
                if (z_raw < 0)
                    cur_zbuf <= 16'd0;
                else if (z_raw > 32'sh0FFFF)
                    cur_zbuf <= 16'hFFFF;
                else
                    cur_zbuf <= z_raw[15:0];
            end
            state <= ST_PX_ZTEST;
        end

        ST_PX_ZTEST: begin
            zb_word_saved <= zb_bram_rdata;
            begin : blk_ztest
                reg [15:0] old_z;
                old_z = cur_zb_idx[0] ? zb_bram_rdata[31:16] : zb_bram_rdata[15:0];
                if (cfg_z_test && (cur_zbuf > old_z)) begin
                    ztest_reject <= ztest_reject + 1;
                    state <= ST_PX_NEXT;
                end else begin
                    ztest_pass <= ztest_pass + 1;
                    rcp_w <= rcp_final;
                    if (cfg_mode == 2'd2) begin
                        mul_a <= cur_r;
                        mul_b <= rcp_final;
                        mul_return <= ST_PX_TEXR;
                        state <= ST_MUL_WAIT;
                    end else begin
                        mul_a <= cur_u;
                        mul_b <= rcp_final;
                        mul_return <= ST_PX_U_DONE;
                        state <= ST_MUL_WAIT;
                    end
                end
            end
        end

        ST_MUL_WAIT: begin
            state <= mul_return;
        end

        ST_PX_U_DONE: begin
            u_persp <= mul_q16;
            mul_a <= cur_v;
            mul_b <= rcp_w;
            mul_return <= ST_PX_V_DONE;
            state <= ST_MUL_WAIT;
        end

        ST_PX_V_DONE: begin
            v_persp <= mul_q16;
            mul_a <= u_persp;
            mul_b <= cfg_tex_fw;
            mul_return <= ST_PX_TU_DONE;
            state <= ST_MUL_WAIT;
        end

        ST_PX_TU_DONE: begin
            tx_saved <= mul_q16[24:16] & cfg_tex_wrap_w;
            mul_a <= v_persp;
            mul_b <= cfg_tex_fh;
            mul_return <= ST_PX_TV_DONE;
            state <= ST_MUL_WAIT;
        end

        ST_PX_TV_DONE: begin
            begin : blk_texaddr
                reg [8:0] ty;
                ty = mul_q16[24:16] & cfg_tex_wrap_h;
                tex_rd_addr <= (ty << cfg_tex_log2_w) | tx_saved;
            end
            if (cfg_mode == 2'd0) begin
                // texrgb: overlap tex BRAM read with r*rcp_w multiply
                mul_a <= cur_r;
                mul_b <= rcp_w;
            end
            mul_return <= ST_PX_TEXR;
            state <= ST_MUL_WAIT;
        end

        ST_PX_TEXR: begin
            if (cfg_mode == 2'd0) begin
                // texrgb: tex ready + R multiply ready
                tex_color <= tex_rd_data;
                begin : blk_clamp_r0
                    reg signed [31:0] r_val;
                    r_val = mul_q16;
                    r_byte <= (r_val[31]) ? 8'd0 : (r_val[31:16] > 16'd255) ? 8'd255 : r_val[23:16];
                end
                mul_a <= cur_g;
                mul_b <= rcp_w;
                mul_return <= ST_PX_G_DONE;
                state <= ST_MUL_WAIT;
            end else if (cfg_mode == 2'd1) begin
                // tex-only: pixel = texture color, convert RGBA32 → RGB332
                begin : blk_tex_only
                    reg [7:0] tr, tg, tb;
                    tr = tex_rd_data[7:0];
                    tg = tex_rd_data[15:8];
                    tb = tex_rd_data[23:16];
                    pixel_out_332 <= {tr[7:5], tg[7:5], tb[7:6]};
                end
                state <= ST_PX_WR;
            end else begin
                // rgb: R multiply ready (no texture)
                begin : blk_clamp_r2
                    reg signed [31:0] r_val;
                    r_val = mul_q16;
                    r_byte <= (r_val[31]) ? 8'd0 : (r_val[31:16] > 16'd255) ? 8'd255 : r_val[23:16];
                end
                mul_a <= cur_g;
                mul_b <= rcp_w;
                mul_return <= ST_PX_G_DONE;
                state <= ST_MUL_WAIT;
            end
        end

        ST_PX_G_DONE: begin
            begin : blk_clamp_g
                reg signed [31:0] g_val;
                g_val = mul_q16;
                g_byte <= (g_val[31]) ? 8'd0 : (g_val[31:16] > 16'd255) ? 8'd255 : g_val[23:16];
            end
            mul_a <= cur_b;
            mul_b <= rcp_w;
            mul_return <= ST_PX_B_DONE;
            state <= ST_MUL_WAIT;
        end

        ST_PX_B_DONE: begin
            begin : blk_combine
                reg [7:0] b_final, r_out, g_out, b_out;
                reg signed [31:0] b_val;
                b_val = mul_q16;
                b_final = (b_val[31]) ? 8'd0 : (b_val[31:16] > 16'd255) ? 8'd255 : b_val[23:16];
                b_byte <= b_final;
                if (cfg_mode == 2'd0) begin
                    // texrgb: tex * vertex color
                    r_out = (tex_color[7:0]   * r_byte)  >> 8;
                    g_out = (tex_color[15:8]  * g_byte)  >> 8;
                    b_out = (tex_color[23:16] * b_final) >> 8;
                end else begin
                    // rgb: vertex color only
                    r_out = r_byte;
                    g_out = g_byte;
                    b_out = b_final;
                end
                pixel_out_332 <= {r_out[7:5], g_out[7:5], b_out[7:6]};
            end
            state <= ST_PX_WR;
        end

        ST_PX_WR: begin
            // Write RGB332 pixel to FB BRAM using byte enables (no read-modify-write)
            fb_bram_addr <= cur_fb_idx[14:2];
            fb_bram_wdata <= {pixel_out_332, pixel_out_332, pixel_out_332, pixel_out_332};
            case (cur_fb_idx[1:0])
                2'd0: fb_bram_wstrb <= 4'b0001;
                2'd1: fb_bram_wstrb <= 4'b0010;
                2'd2: fb_bram_wstrb <= 4'b0100;
                2'd3: fb_bram_wstrb <= 4'b1000;
            endcase

            // Write z-value to ZB BRAM (2 values per 32-bit word)
            if (cfg_z_write) begin
                zb_bram_addr <= cur_zb_idx[14:1];
                if (cur_zb_idx[0]) begin
                    zb_bram_wdata <= {cur_zbuf, zb_word_saved[15:0]};
                    zb_bram_wstrb <= 4'b1100;
                end else begin
                    zb_bram_wdata <= {zb_word_saved[31:16], cur_zbuf};
                    zb_bram_wstrb <= 4'b0011;
                end
            end
            state <= ST_PX_NEXT;
        end

        ST_PX_NEXT: begin
            px_idx <= px_idx + 1;
            cur_fb_idx <= cur_fb_idx + 1;
            cur_zb_idx <= cur_zb_idx + 1;
            cur_z <= cur_z + cfg_dp_z;
            cur_w <= cur_w + cfg_dp_w;
            cur_u <= cur_u + cfg_dp_u;
            cur_v <= cur_v + cfg_dp_v;
            cur_r <= cur_r + cfg_dp_r;
            cur_g <= cur_g + cfg_dp_g;
            cur_b <= cur_b + cfg_dp_b;
            state <= ST_PX_ZSETUP;
        end

        default: state <= ST_FIFO_IDLE;

        endcase
    end
end

endmodule
