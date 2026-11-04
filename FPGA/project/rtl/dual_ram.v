`timescale 1ns / 1ps
// One synchronous read stage, identical in the behavioral model and vendor IP.
// 跨时钟缓存基础模块：dual_ram 为双端口 RAM，dual_reset 同步释放复位，dual_fifo 管理队列指针。
// 仿真 RAM 与厂商 RAM 都使用一拍同步读；调用方需保留这一拍延迟。
module dual_ram #(
    parameter W = 256, A = 9
) (
    input wc,
    input we,
    input [A-1:0] wa,
    input [W-1:0] wd,
    input rc,
    input [A-1:0] ra,
    output [W-1:0] rd
);

    // SIMULATION 使用行为模型；硬件实现使用厂商双端口 RAM，二者读延迟相同。
`ifdef SIMULATION
    reg [W-1:0] mem[0:(1<<A)-1];
    reg [W-1:0] q;
    always @(posedge wc) if (we) mem[wa] <= wd;
    always @(posedge rc) q <= mem[ra];
    assign rd = q;
`else
    ipml_sdpram_v1_6_wr_fram_buf #(
        .c_WR_ADDR_WIDTH(A),
        .c_WR_DATA_WIDTH(W),
        .c_RD_ADDR_WIDTH(A),
        .c_RD_DATA_WIDTH(W),
        .c_OUTPUT_REG(0),
        .c_RD_OCE_EN(0),
        .c_WR_CLK_EN(0),
        .c_RD_CLK_EN(0),
        .c_RESET_TYPE("ASYNC_RESET"),
        .c_POWER_OPT(0),
        .c_INIT_FILE("NONE"),
        .c_INIT_FORMAT("BIN"),
        .c_WR_BYTE_EN(0),
        .c_BE_WIDTH(1)
    ) ram (
        .wr_clk(wc),
        .wr_en(we),
        .wr_addr(wa),
        .wr_data(wd),
        .wr_clk_en(1'b1),
        .wr_rst(1'b0),
        .wr_byte_en(1'b1),
        .wr_addr_strobe(1'b0),
        .rd_clk(rc),
        .rd_addr(ra),
        .rd_data(rd),
        .rd_clk_en(1'b1),
        .rd_rst(1'b0),
        .rd_oce(1'b0),
        .rd_addr_strobe(1'b0)
    );
`endif
endmodule

// 异步拉低、三拍同步释放的复位；只有本时钟域看到连续时钟后 rstn 才能拉高。
module dual_reset (
    input clk,
    input arstn,
    output rstn
);
    (* ASYNC_REG="TRUE" *) reg [2:0] sync;
    always @(posedge clk or negedge arstn)
        if (!arstn) sync <= 0;
        else sync <= {sync[1:0], 1'b1};
    assign rstn = sync[2];
endmodule

// Gray pointers with registered RAM look-ahead. Consumer may pop once/clock.
module dual_fifo #(
    parameter W = 258, A = 9
) (
    input wc,
    input wrstn,
    input push,
    input [W-1:0] din,
    output full,
    input rc,
    input rrstn,
    input pop,
    output [W-1:0] dout,
    output empty
);

    // wp/rp 是二进制读写指针；wg/rg 是 Gray 编码，跨时钟只同步 Gray 指针。
    reg [A:0] wp, wg, rp, rg;
    (* ASYNC_REG="TRUE" *) reg [A:0] rg1, rg2, wg1, wg2;
    wire [A:0] wn = wp + 1'b1;
    wire [A:0] wgn = (wn >> 1) ^ wn;
    wire [A:0] rn = rp + 1'b1;

    // 满判断比较下一写指针，保留一个槽位；不要与网络整包队列的“当前指针满判断”混淆。
    assign full  = (wgn == {~rg2[A:A-1], rg2[A-2:0]});
    assign empty = (rg == wg2);

    // RAM 提前读取下一读地址，配合同步读延迟，使连续 pop 时每拍都能消费当前字。
    wire take = pop && !empty;
    dual_ram #(
        .W(W),
        .A(A)
    ) ram (
        .wc(wc),
        .we(push && !full),
        .wa(wp[A-1:0]),
        .wd(din),
        .rc(rc),
        .ra(take ? rn[A-1:0] : rp[A-1:0]),
        .rd(dout)
    );
    always @(posedge wc or negedge wrstn) begin
        if (!wrstn) begin
            wp  <= 0;
            wg  <= 0;
            rg1 <= 0;
            rg2 <= 0;
        end else begin
            rg1 <= rg;
            rg2 <= rg1;
            if (push && !full) begin
                wp <= wn;
                wg <= wgn;
            end
        end
    end

    always @(posedge rc or negedge rrstn) begin
        if (!rrstn) begin
            rp  <= 0;
            rg  <= 0;
            wg1 <= 0;
            wg2 <= 0;
        end else begin
            wg1 <= wg;
            wg2 <= wg1;
            if (take) begin
                rp <= rn;
                rg <= (rn >> 1) ^ rn;
            end
        end
    end
endmodule
