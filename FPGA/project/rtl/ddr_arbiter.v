`timescale 1ns / 1ps

// One outstanding native DDR transaction in each direction. Independent
// round-robin read/write grants remain locked through the final data beat.
// 四路 DDR 访问仲裁：读和写各自轮询，且各自最多保持一个未完成事务。
// 地址高位划分通道区域；一旦握手成功，数据返回始终交给锁定的通道。
module ddr_arbiter (
    input clk, rstn,
    input [111:0] s_awaddr,
    input [3:0] s_awvalid,
    output reg [3:0] s_awready,
    input [1023:0] s_wdata,
    output [3:0] s_wready, s_wlast,
    input [111:0] s_araddr,
    input [3:0] s_arvalid,
    output reg [3:0] s_arready,
    output [3:0] s_rvalid, s_rlast,
    output [27:0] awaddr,
    output awvalid,
    input awready,
    output [255:0] wdata,
    input wready, wlast,
    output [27:0] araddr,
    output arvalid,
    input arready,
    input rvalid, rlast
);

    // wa/ra 标记在途写/读事务；wo/ro 锁定其通道；wt/rt 是下一次轮询起点。
    reg wa, ra;
    reg [1:0] wo, ro, wt, rt;
    reg [1:0] ws, rs, idx;
    reg wf, rf;
    integer i;

    // 从轮询起点检查四路请求，找到第一个有效通道；未完成事务期间不给新地址授权。
    always @* begin
        ws  = wt;
        rs  = rt;
        wf  = 0;
        rf  = 0;
        idx = 0;
        for (i = 0; i < 4; i = i + 1) begin
            idx = wt + i;
            if (!wf && s_awvalid[idx]) begin
                ws = idx;
                wf = 1;
            end
        end
        for (i = 0; i < 4; i = i + 1) begin
            idx = rt + i;
            if (!rf && s_arvalid[idx]) begin
                rs = idx;
                rf = 1;
            end
        end
        s_awready = 0;
        s_arready = 0;
        if (!wa && wf) s_awready[ws] = awready;
        if (!ra && rf) s_arready[rs] = arready;
    end

    // 每路占 2^21 个 32-bit 地址单位，即 8MiB；高位通道号与通道内 bank 地址相加。
    assign awaddr = {5'd0, ws, 21'd0} + s_awaddr[ws*28+:28];
    assign araddr = {5'd0, rs, 21'd0} + s_araddr[rs*28+:28];
    assign awvalid = !wa && wf;
    assign arvalid = !ra && rf;
    assign wdata = s_wdata[wo*256+:256];
    assign s_wready = wa && wready ? (4'b1 << wo) : 0;
    assign s_wlast = wa && wlast ? (4'b1 << wo) : 0;
    assign s_rvalid = ra && rvalid ? (4'b1 << ro) : 0;
    assign s_rlast = ra && rlast ? (4'b1 << ro) : 0;

    // 地址握手锁定通道，最后一个数据拍完成后才释放，并把轮询起点移至下一路。
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            wa <= 0;
            ra <= 0;
            wo <= 0;
            ro <= 0;
            wt <= 0;
            rt <= 0;
        end else begin
            if (awvalid && awready) begin
                wa <= 1;
                wo <= ws;
            end
            if (wa && wready && wlast) begin
                wa <= 0;
                wt <= wo + 1'b1;
            end
            if (arvalid && arready) begin
                ra <= 1;
                ro <= rs;
            end
            if (ra && rvalid && rlast) begin
                ra <= 0;
                rt <= ro + 1'b1;
            end
        end
    end
endmodule
