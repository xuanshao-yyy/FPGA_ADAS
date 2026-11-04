`timescale 1ns / 1ps

// Three 2MiB banks. Publish only complete frames; retain a display bank for a
// whole HDMI frame. Pango native interface addresses count 32-bit words.
// 单路 DDR 三缓冲：wb 为写入 bank，rb 为最新完整 bank，db 为当前显示 bank。
// 只有整帧写完才发布；frame_req 在帧边界选择 bank，line_req 请求下一行缓存。
module tile_store #(
    parameter FRAME_WORDS = 14400, LINE_WORDS = 40, PRODUCT = 0, ROTATE_180 = 0, PANORAMA_REAR = 0
) (
    input clk, rstn,
    input [257:0] q,
    input empty,
    output pop,
    output reg [27:0] awaddr,
    output [3:0] awlen,
    output awvalid,
    input awready,
    output [255:0] wdata,
    input wready, wlast,
    output reg [27:0] araddr,
    output [3:0] arlen,
    output arvalid,
    input arready,
    input [255:0] rdata,
    input rvalid, rlast,
    input frame_req,
    input line_req,
    input [9:0] line_row,
    output reg line_ack,
    output reg [23:0] line_tags,
    output line_we,
    output [7:0] line_addr,
    output [255:0] line_data,
    output reg [31:0] completed,
    output reg [31:0] display_seq,
    output reg display_valid,
    output reg fault,
    input parking,output [95:0] diagnostic,input panorama
);

    // rv 表示有新完整帧等待显示；wa 表示正在接收一帧；written 的单位是 256-bit 数据字。
    reg [1:0] wb, rb, db;
    reg rv, wa;
    reg [31:0] ready_seq;
    reg [15:0] written;
    reg [255:0] burst[0:7];
    reg [2:0] fill, beat;
    reg eof;

    // 写状态 ws：0 收集 8 拍，1 等地址握手，2 发数据；读状态 rs：0 等行请求，1 发地址，2 收数据，3 确认。
    reg [1:0] ws, rs;
    reg [2:0] fr_sync, lr_sync;
    reg [9:0] row;
    reg epoch, job_epoch, job_valid;
    (* ASYNC_REG="TRUE" *) reg [1:0] park_sync;
    reg frame_park;
    (* ASYNC_REG="TRUE" *) reg [1:0] pano_sync;reg frame_pano;
    wire [9:0] pano_row=line_row>>1;
    wire [9:0] rear_row=(pano_row>=45&&pano_row<315)?((pano_row-45)*4+2)/3:0;
    wire [9:0] sampled_row=frame_pano ? ((PANORAMA_REAR?rear_row:pano_row)<<1) : frame_park ? ((line_row>=360 ? line_row-360 : line_row)<<1) : line_row;
    // Rotate in the full-resolution read path, before the existing PARK decimation.
    // Cache tags/parity remain in output coordinates; frame ownership is unchanged.
    localparam [9:0] LAST_ROW=FRAME_WORDS/LINE_WORDS-1;
    wire [9:0] physical_row=ROTATE_180 ? LAST_ROW-sampled_row : sampled_row;
    reg [6:0] word_no;
    localparam [6:0] LAST_WORD=LINE_WORDS-1;
    wire [6:0] cache_word=ROTATE_180 ? LAST_WORD-word_no : word_no;
    wire new_frame = fr_sync[2] != epoch;
    wire new_line = lr_sync[2] != line_ack;
    assign diagnostic={completed,display_seq,{31'd0,fault}};
    assign pop = (ws == 0) && !empty;
    assign awlen = 7;
    assign awvalid = (ws == 1);
    assign wdata = burst[beat];
    assign arlen = 7;
    assign arvalid = (rs == 1);
    assign line_we = (rs == 2) && rvalid;
    assign line_addr = {row[0], cache_word};
    // Reverse both the word order and the sixteen RGB565 lanes in each word.
    // Constant generate wiring: no extra DDR transaction or per-pixel CPU work.
    genvar lane;
    generate for(lane=0;lane<16;lane=lane+1)begin: oriented_pixels
        assign line_data[lane*16+:16]=ROTATE_180 ? rdata[(15-lane)*16+:16] : rdata[lane*16+:16];
    end endgenerate

    // One explicit RAM write port. PDS 2022.2-SP6.4 inferred the wrong address
    // mux when burst[0] and burst[fill] were assigned in separate FSM branches.
    wire burst_we = rstn && pop && (q[256] || wa);
    wire [2:0] burst_waddr = q[256] ? 3'd0 : fill;

    always @(posedge clk) if (burst_we) burst[burst_waddr] <= q[255:0];

    // 选择未被“最新完整帧”和“当前显示帧”占用的 bank，避免覆盖正在显示的画面。
    function [1:0] free_bank;
        input ready;
        input [1:0] latest;
        input active;
        input [1:0] display;
        begin
            if ((!ready || latest != 0) && (!active || display != 0)) free_bank = 0;
            else if ((!ready || latest != 1) && (!active || display != 1)) free_bank = 1;
            else free_bank = 2;
        end
    endfunction

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            wb <= 0;
            rb <= 0;
            db <= 0;
            rv <= 0;
            wa <= 0;
            written <= 0;
            fill <= 0;
            beat <= 0;
            eof <= 0;
            ws <= 0;
            rs <= 0;
            awaddr <= 0;
            araddr <= 0;
            completed <= 0;
            ready_seq <= 0;
            display_seq <= 0;
            display_valid <= 0;
            fault <= 0;
            fr_sync <= 0;
            lr_sync <= 0;
            epoch <= 0;park_sync<=0;frame_park<=0;pano_sync<=0;frame_pano<=0;
            line_ack <= 0;
            line_tags <= 0;
            row <= 0;
            word_no <= 0;
            job_epoch <= 0;
            job_valid <= 0;
        end else begin
            fr_sync <= {fr_sync[1:0], frame_req};
            park_sync <= {park_sync[0],PRODUCT ? parking : 1'b0};
            pano_sync<={pano_sync[0],PRODUCT ? panorama : 1'b0};
            lr_sync <= {lr_sync[1:0], line_req};

            // 帧请求只在没有在途行读取时切换显示 bank；整帧期间 db/display_seq 保持一致。
            if (new_frame && rs == 0) begin
                epoch <= fr_sync[2];frame_park<=park_sync[1];frame_pano<=pano_sync[1];
                line_tags <= 0;
                if (rv) begin
                    db <= rb;
                    display_valid <= 1;
                    display_seq <= ready_seq;
                    rv <= 0;
                end
            end

            // 写入路径：先收集 8 个 256-bit 字，再发出一个长度为 8 的 DDR 突发。
            case (ws)
                0: if (!empty) begin
                    if (q[256]) begin
                        wb <= free_bank(rv, rb, display_valid, db);
                        wa <= 1;
                        written <= 0;
                        fill <= 1;
                    end else if (wa) begin
                        fill <= fill + 1'b1;
                        if (fill == 7) begin
                            awaddr <= {7'd0, wb, 19'd0} + ({12'd0, written} << 3);
                            eof <= q[257];
                            beat <= 0;
                            ws <= 1;
                        end
                    end
                end
                1: if (awready) ws <= 2;
                2: if (wready) begin
                    beat <= beat + 1'b1;
                    if (wlast) begin
                        if (beat != 7) fault <= 1;
                        written <= written + 8;
                        ws <= 0;

                        // 仅帧尾标志与总写入字数同时正确时发布新帧序号；半帧不会供显示或网络使用。
                        if (eof) begin
                            wa <= 0;
                            if (written == FRAME_WORDS - 8) begin
                                rb <= wb;
                                rv <= 1;
                                completed <= completed + 1;
                                ready_seq <= completed + 1;
                            end else fault <= 1;
                        end else if (written >= FRAME_WORDS - 8) wa <= 0;
                    end
                end
            endcase

            // 读取路径：每行分多个 8 拍突发读入双行 RAM，完成后发布标签并翻转确认。
            case (rs)
                0: if (new_line && !new_frame) begin
                    row <= line_row;
                    word_no <= 0;
                    job_epoch <= epoch;
                    job_valid <= display_valid;
                    araddr <= {7'd0, db, 19'd0} + physical_row * (LINE_WORDS * 8);
                    if (display_valid) rs <= 1;
                    else rs <= 3;
                end
                1: if (arready) rs <= 2;
                2: if (rvalid) begin
                    word_no <= word_no + 1'b1;
                    if (rlast) begin
                        if (word_no[2:0] != 7) fault <= 1;
                        if (word_no == LINE_WORDS - 1) rs <= 3;
                        else begin
                            araddr <= araddr + 64;
                            rs <= 1;
                        end
                    end
                end

                // 行标签每组 12 bit：{有效, 帧 epoch, 10-bit 行号}；奇偶行分别占用一组标签。
                3: begin
                    line_tags[row[0]*12+:12] <= {job_valid, job_epoch, row};
                    line_ack <= lr_sync[2];
                    rs <= 0;
                end
            endcase
        end
    end
endmodule
