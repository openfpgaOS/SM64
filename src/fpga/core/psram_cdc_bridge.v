`default_nettype none

// Single-outstanding CDC bridge for psram_controller word interface.
// Source side (src_clk): CPU/arbiter domain.
// Destination side (dst_clk): PSRAM controller domain.
module psram_cdc_bridge (
    input  wire        src_clk,
    input  wire        src_reset_n,
    input  wire        src_word_rd,
    input  wire        src_word_wr,
    input  wire [21:0] src_word_addr,
    input  wire [31:0] src_word_data,
    input  wire [3:0]  src_word_wstrb,
    output reg  [31:0] src_word_q,
    output wire        src_word_busy,
    output reg         src_word_q_valid,

    input  wire        dst_clk,
    input  wire        dst_reset_n,
    output reg         dst_word_rd,
    output reg         dst_word_wr,
    output reg  [21:0] dst_word_addr,
    output reg  [31:0] dst_word_data,
    output reg  [3:0]  dst_word_wstrb,
    input  wire [31:0] dst_word_q,
    input  wire        dst_word_busy,
    input  wire        dst_word_q_valid
);

// ---------------------------
// Source domain request latch
// ---------------------------
reg        src_pending;
reg [21:0] src_req_addr;
reg [31:0] src_req_data;
reg [3:0]  src_req_wstrb;
reg        src_req_is_read;
reg        src_req_toggle;
reg        src_req_level_d;

assign src_word_busy = src_pending;

// ------------------------------
// Source->dest synchronizer buses
// ------------------------------
reg [21:0] src_req_addr_sync1, src_req_addr_sync2;
reg [31:0] src_req_data_sync1, src_req_data_sync2;
reg [3:0]  src_req_wstrb_sync1, src_req_wstrb_sync2;
reg        src_req_is_read_sync1, src_req_is_read_sync2;

// -----------------------------
// Dest domain request handling
// -----------------------------
reg req_toggle_sync1, req_toggle_sync2;
reg req_toggle_seen;
wire req_event = (req_toggle_sync2 != req_toggle_seen);

reg dst_op_active;
reg dst_op_issued;
reg dst_op_is_read;
reg dst_op_seen_busy;
reg dst_req_wait_data;

// -----------------------------
// Dest->source completion signal
// -----------------------------
reg        dst_done_toggle;
reg        dst_done_is_read;
reg [31:0] dst_done_rdata;

reg done_toggle_sync1, done_toggle_sync2;
reg done_toggle_seen;
reg done_is_read_sync1, done_is_read_sync2;
reg [31:0] done_rdata_sync1, done_rdata_sync2;
wire done_event = (done_toggle_sync2 != done_toggle_seen);
reg src_done_wait_data;

// Source domain: latch one request and wait for completion.
always @(posedge src_clk or negedge src_reset_n) begin
    if (!src_reset_n) begin
        src_pending <= 1'b0;
        src_req_addr <= 22'd0;
        src_req_data <= 32'd0;
        src_req_wstrb <= 4'd0;
        src_req_is_read <= 1'b0;
        src_req_toggle <= 1'b0;
        src_req_level_d <= 1'b0;
        src_word_q <= 32'd0;
        src_word_q_valid <= 1'b0;
        done_toggle_sync1 <= 1'b0;
        done_toggle_sync2 <= 1'b0;
        done_toggle_seen <= 1'b0;
        done_is_read_sync1 <= 1'b0;
        done_is_read_sync2 <= 1'b0;
        done_rdata_sync1 <= 32'd0;
        done_rdata_sync2 <= 32'd0;
        src_done_wait_data <= 1'b0;
    end else begin
        src_word_q_valid <= 1'b0;

        done_toggle_sync1 <= dst_done_toggle;
        done_toggle_sync2 <= done_toggle_sync1;
        done_is_read_sync1 <= dst_done_is_read;
        done_is_read_sync2 <= done_is_read_sync1;
        done_rdata_sync1 <= dst_done_rdata;
        done_rdata_sync2 <= done_rdata_sync1;

        src_req_level_d <= (src_word_rd || src_word_wr);

        if (!src_pending) begin
            if (done_event) begin
                done_toggle_seen <= done_toggle_sync2;
            end
            // Edge-detect request level so level-held requesters (bridge path)
            // generate only one transaction.
            if ((src_word_rd || src_word_wr) && !src_req_level_d) begin
                src_req_addr <= src_word_addr;
                src_req_data <= src_word_data;
                src_req_wstrb <= src_word_wstrb;
                src_req_is_read <= src_word_rd;
                src_req_toggle <= ~src_req_toggle;
                done_toggle_seen <= done_toggle_sync2;
                src_pending <= 1'b1;
            end
        end else if (src_done_wait_data) begin
            if (done_is_read_sync2) begin
                src_word_q <= done_rdata_sync2;
                src_word_q_valid <= 1'b1;
            end
            src_pending <= 1'b0;
            src_done_wait_data <= 1'b0;
        end else if (done_event) begin
            done_toggle_seen <= done_toggle_sync2;
            // Let done_* synchronizers settle for one additional src clock.
            src_done_wait_data <= 1'b1;
        end
    end
end

// Destination domain: execute one request against psram_controller.
always @(posedge dst_clk or negedge dst_reset_n) begin
    if (!dst_reset_n) begin
        src_req_addr_sync1 <= 22'd0;
        src_req_addr_sync2 <= 22'd0;
        src_req_data_sync1 <= 32'd0;
        src_req_data_sync2 <= 32'd0;
        src_req_wstrb_sync1 <= 4'd0;
        src_req_wstrb_sync2 <= 4'd0;
        src_req_is_read_sync1 <= 1'b0;
        src_req_is_read_sync2 <= 1'b0;
        req_toggle_sync1 <= 1'b0;
        req_toggle_sync2 <= 1'b0;
        req_toggle_seen <= 1'b0;

        dst_word_rd <= 1'b0;
        dst_word_wr <= 1'b0;
        dst_word_addr <= 22'd0;
        dst_word_data <= 32'd0;
        dst_word_wstrb <= 4'd0;

        dst_op_active <= 1'b0;
        dst_op_issued <= 1'b0;
        dst_op_is_read <= 1'b0;
        dst_op_seen_busy <= 1'b0;
        dst_req_wait_data <= 1'b0;

        dst_done_toggle <= 1'b0;
        dst_done_is_read <= 1'b0;
        dst_done_rdata <= 32'd0;
    end else begin
        dst_word_rd <= 1'b0;
        dst_word_wr <= 1'b0;

        src_req_addr_sync1 <= src_req_addr;
        src_req_addr_sync2 <= src_req_addr_sync1;
        src_req_data_sync1 <= src_req_data;
        src_req_data_sync2 <= src_req_data_sync1;
        src_req_wstrb_sync1 <= src_req_wstrb;
        src_req_wstrb_sync2 <= src_req_wstrb_sync1;
        src_req_is_read_sync1 <= src_req_is_read;
        src_req_is_read_sync2 <= src_req_is_read_sync1;
        req_toggle_sync1 <= src_req_toggle;
        req_toggle_sync2 <= req_toggle_sync1;

        if (!dst_op_active && req_event) begin
            req_toggle_seen <= req_toggle_sync2;
            // Let request payload synchronizers settle for one additional dst clock.
            dst_req_wait_data <= 1'b1;
        end else if (!dst_op_active && dst_req_wait_data) begin
            dst_word_addr <= src_req_addr_sync2;
            dst_word_data <= src_req_data_sync2;
            dst_word_wstrb <= src_req_wstrb_sync2;
            dst_op_is_read <= src_req_is_read_sync2;
            dst_op_active <= 1'b1;
            dst_op_issued <= 1'b0;
            dst_op_seen_busy <= 1'b0;
            dst_req_wait_data <= 1'b0;
        end else if (dst_op_active) begin
            if (!dst_op_issued) begin
                if (!dst_word_busy) begin
                    dst_word_rd <= dst_op_is_read;
                    dst_word_wr <= !dst_op_is_read;
                    dst_op_issued <= 1'b1;
                    dst_op_seen_busy <= 1'b0;
                end
            end else if (dst_op_is_read) begin
                if (dst_word_q_valid) begin
                    dst_done_is_read <= 1'b1;
                    dst_done_rdata <= dst_word_q;
                    dst_done_toggle <= ~dst_done_toggle;
                    dst_op_active <= 1'b0;
                    dst_op_issued <= 1'b0;
                end
            end else begin
                if (!dst_op_seen_busy && dst_word_busy) begin
                    dst_op_seen_busy <= 1'b1;
                end else if (dst_op_seen_busy && !dst_word_busy) begin
                    dst_done_is_read <= 1'b0;
                    dst_done_rdata <= 32'd0;
                    dst_done_toggle <= ~dst_done_toggle;
                    dst_op_active <= 1'b0;
                    dst_op_issued <= 1'b0;
                    dst_op_seen_busy <= 1'b0;
                end
            end
        end
    end
end

endmodule
