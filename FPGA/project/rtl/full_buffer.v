`timescale 1ns / 1ps

// Four independent frame stores share the native DDR controller.
// Each store writes only its assigned address region, publishes whole frames,
// and reads 80 x 256-bit words for each full 1280-pixel output line.
// 四路完整画面的 DDR 与行缓存组合；每路各有独立 tile_store，但共用 DDR 控制器。
// 每行 1280 个 RGB565 像素，即 80 个 256-bit 数据字；source_seq 标识各路实际采集帧。
module full_buffer #(parameter PRODUCT=0) (
    input clk, rstn, vclk, vrstn,
    input [1031:0] q,
    input [3:0] empty,
    output [3:0] pop,
    output [27:0] awaddr,
    output [3:0] awlen,
    output awvalid,
    input awready,
    output [255:0] wdata,
    input wready, wlast,
    output [27:0] araddr,
    output [3:0] arlen,
    output arvalid,
    input arready,
    input [255:0] rdata,
    input rvalid, rlast,
    output [3:0] available,
    output fault,
    output hs, vs, de,
    output [63:0] pixels,
    output [127:0] source_seq,
    input parking,output boundary,output [31:0] frame_count,output [1:0] quad_select,output [95:0] diagnostic_ch2,input panorama
);
    wire [111:0] saw, sar;
    wire [3:0] sav, savready, srv, srready, swready, swlast, srvalid, srlast;
    wire [1023:0] swdata, read_lines;
    wire fr, lr;
    wire [3:0] ack, errors;
    wire [9:0] row;
    wire [95:0] tags;
    wire [31:0] ra;
    wire uf;
    genvar c;

    // 每路独立管理帧 bank 和双行 RAM；行 RAM 的高地址位由行号奇偶决定。
    generate
        for (c = 0; c < 4; c = c + 1) begin : channels
            wire we;wire [95:0] probe;
            if(c==2)assign diagnostic_ch2=probe;
            wire [7:0] wa;
            wire [255:0] wd;
            tile_store #(
                .PRODUCT(PRODUCT),
                .ROTATE_180(c==1),.PANORAMA_REAR(c==3),
                .FRAME_WORDS(57600),
                .LINE_WORDS (80)
            ) store (
                .clk(clk),
                .rstn(rstn),
                .q(q[c*258+:258]),
                .empty(empty[c]),
                .pop(pop[c]),
                .awaddr(saw[c*28+:28]),
                .awlen(),
                .awvalid(sav[c]),
                .awready(savready[c]),
                .wdata(swdata[c*256+:256]),
                .wready(swready[c]),
                .wlast(swlast[c]),
                .araddr(sar[c*28+:28]),
                .arlen(),
                .arvalid(srv[c]),
                .arready(srready[c]),
                .rdata(rdata),
                .rvalid(srvalid[c]),
                .rlast(srlast[c]),
                .diagnostic(probe),.parking(parking),.panorama(panorama),
                .frame_req(fr),
                .line_req(lr),
                .line_row(row),
                .line_ack(ack[c]),
                .line_tags(tags[c*24+:24]),
                .line_we(we),
                .line_addr(wa),
                .line_data(wd),
                .completed(),
                .display_seq(source_seq[c*32+:32]),
                .display_valid(available[c]),
                .fault(errors[c])
            );
            dual_ram #(
                .W(256),
                .A(8)
            ) ram (
                .wc(clk),
                .we(we),
                .wa(wa),
                .wd(wd),
                .rc(vclk),
                .ra(ra[c*8+:8]),
                .rd(read_lines[c*256+:256])
            );
        end
    endgenerate

    // 四路 store 的地址请求送入公共仲裁器；返回数据按在途事务通道分发。
    ddr_arbiter arb (
        .clk(clk),
        .rstn(rstn),
        .s_awaddr(saw),
        .s_awvalid(sav),
        .s_awready(savready),
        .s_wdata(swdata),
        .s_wready(swready),
        .s_wlast(swlast),
        .s_araddr(sar),
        .s_arvalid(srv),
        .s_arready(srready),
        .s_rvalid(srvalid),
        .s_rlast(srlast),
        .awaddr(awaddr),
        .awvalid(awvalid),
        .awready(awready),
        .wdata(wdata),
        .wready(wready),
        .wlast(wlast),
        .araddr(araddr),
        .arvalid(arvalid),
        .arready(arready),
        .rvalid(rvalid),
        .rlast(rlast)
    );
    assign awlen = 7;
    assign arlen = 7;
    assign fault = (|errors) || uf;

    // 显示时序通过 fr/lr 翻转握手请求帧和行；ack/tags 返回行号、帧 epoch 与有效标志。
    full_display #(.PRODUCT(PRODUCT)) display (
        .panorama(panorama),.source_seq(source_seq),.ram_addrs(ra),
        .parking(parking),.boundary(boundary),.frame_count(frame_count),.quad_select(quad_select),
        .clk(vclk),
        .rstn(vrstn),
        .line_ack(ack),
        .line_tags(tags),
        .ram_data(read_lines),
        .frame_req(fr),
        .line_req(lr),
        .line_row(row),
        .ram_addr(),
        .hs(hs),
        .vs(vs),
        .de(de),
        .pixels(pixels),
        .underflow(uf)
    );
endmodule
