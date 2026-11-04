`timescale 1ns / 1ps

// One physical Ethernet interface serving two independent video streams.
// Each stream owns its queue. The round-robin mux locks its selection until
// the MAC releases the complete packet, including the Ethernet FCS.
// 一个 SFP 网络口承载两路独立视频队列；两路共享 MAC、对端 ARP 信息和 PCS 链路。
// source_seq 在输出帧消隐期间更新，再用 video_clk 两拍采样供新帧选择。
module network_port #(
    parameter PRODUCT=0,CONTROL=0,PRIMARY=0,
    parameter [47:0] LOCAL_MAC = 48'h025000000a02,
    parameter [31:0] LOCAL_IP = 32'hc0a80a02, PEER_IP = 32'hc0a80a01,
    parameter FIRST_PORT = 5000,
    parameter IMAGE_BYTES = 1440,
    parameter SLOT_BITS = 3,
    parameter WORD_BITS = $clog2(IMAGE_BYTES / 4)
) (
    input nclk, arstn, synced,
    input [7:0] rxd,
    input rxk, rxerr,
    output reg [7:0] txd,
    output reg txk, disparity,
    input vclk, vrstn, vs, de,
    input [31:0] pixels,
    input [63:0] source_seq,
    output link, known, fault,
    output [31:0] sent,
    input [767:0] diagnostic_data,
    input parking,input panorama,input [15:0] quad_pixel,input [31:0] raster_frame,
    input applied_toggle,input [1:0] actual_mode,input [31:0] applied_seq,applied_frame,
    output mode_toggle,output [1:0] desired_mode,output [31:0] desired_seq
);

    // 网络复位与视频复位组合后，分别在写入/读出时钟域同步释放整包队列的两端。
    wire nrstn, rvrstn, rnrstn;
    dual_reset nreset (
        nclk,
        arstn,
        nrstn
    );
    dual_reset rvreset (
        vclk,
        arstn && vrstn,
        rvrstn
    );
    dual_reset rnreset (
        nclk,
        arstn && vrstn,
        rnrstn
    );
    (* ASYNC_REG="TRUE" *) reg [1:0] sync_status;

    always @(posedge nclk or negedge nrstn)
        if (!nrstn) sync_status <= 0;
        else sync_status <= {sync_status[0], synced};
    wire [7:0] pcs_txd, mac_txd, mac_rxd;
    wire pcs_txk, pcs_disp, mac_valid, mac_ready, mac_rxvalid, mac_rxerr;

    always @(posedge nclk) begin
        txd <= pcs_txd;
        txk <= pcs_txk;
        disparity <= pcs_disp;
    end

    // PCS 只在网络时钟域运行；链路协商结果送给 MAC，实际字节握手通过 mac_ready。
    base_x_pcs pcs (
        .clk(nclk),
        .rstn(nrstn),
        .synced(sync_status[1]),
        .rxd(rxd),
        .rxk(rxk),
        .rxerr(rxerr),
        .txd(pcs_txd),
        .txk(pcs_txk),
        .idle_disparity(pcs_disp),
        .mac_txd(mac_txd),
        .mac_valid(mac_valid),
        .mac_ready(mac_ready),
        .mac_rxd(mac_rxd),
        .mac_rxvalid(mac_rxvalid),
        .mac_rxerr(mac_rxerr),
        .link(link),
        .sgmii(),
        .parallel_detect()
    );
    wire [1:0] empties, releases, overflow;wire [3:0] composites;
    wire park=PRODUCT ? parking : 1'b0;wire pano=PRODUCT ? panorama : 1'b0;
    wire [1:0] stream_mode=pano?2:park?1:0;
    wire release_packet, empty;
    wire [63:0] data;
    wire [63:0] frames, sources;
    wire [21:0] packets;
    wire [WORD_BITS-1:0] addr;
    wire [31:0] pixel_data;
    wire [31:0] frame_id, source_id;
    wire [10:0] packet_id;
    wire [15:0] port;

    // Stable frame metadata crosses as bundled data: tile_store holds it for
    // the entire output frame; active data starts >1ms after bank selection.
    reg [63:0] seq1, seq2;

    always @(posedge vclk) begin
        seq1 <= source_seq;
        seq2 <= seq1;
    end

    // 两路 ring 各有自己的 RAM、帧号和源帧序号；不会因另一源停帧而重复发送旧图。
    genvar c;
    generate
        for (c = 0; c < 2; c = c + 1) begin : rings
            video_packet_ring #(
                .PRODUCT(PRODUCT),
                .IMAGE_BYTES(IMAGE_BYTES),
                .SLOT_BITS  (SLOT_BITS)
            ) ring (
                .composite(stream_mode),.composite_out(composites[c*2+:2]),
                .vclk(vclk),
                .vrstn(rvrstn),
                .vs(vs),
                .de(de && (!(park||pano) || (PRIMARY && c==0))),
                .pixel(pano ? pixels[15:0] : park ? quad_pixel : pixels[c*16+:16]),
                .source_seq((park||pano) ? raster_frame : seq2[c*32+:32]),
                .nclk(nclk),
                .nrstn(rnrstn),
                .release_packet(releases[c]),
                .pixel_addr(addr),
                .pixel_data(data[c*32+:32]),
                .source_id(sources[c*32+:32]),
                .frame_id(frames[c*32+:32]),
                .packet_id(packets[c*11+:11]),
                .empty(empties[c]),
                .overflow(overflow[c])
            );
        end
    endgenerate

    // 整包轮询后选择对应 UDP 端口；直到 MAC 发完 FCS 才释放所选队列。
    dual_packet_mux #(
        .PRODUCT(PRODUCT),.FIRST_PORT(FIRST_PORT)
    ) mux (
        .composite(composites),
        .clk(nclk),
        .rstn(rnrstn),
        .empty_in(empties),
        .frames(frames),
        .sources(sources),
        .packets(packets),
        .data_in(data),
        .release_packet(release_packet),
        .release_out(releases),
        .empty(empty),
        .frame_id(frame_id),
        .source_id(source_id),
        .packet_id(packet_id),
        .pixel_data(pixel_data),
        .stream_port(port)
    );
    video_udp_mac #(
        .CONTROL(CONTROL),.DIAGNOSTICS(PRIMARY),
        .IMAGE_BYTES(IMAGE_BYTES),
        .LOCAL_MAC(LOCAL_MAC),
        .LOCAL_IP(LOCAL_IP),
        .PEER_IP(PEER_IP)
    ) mac (
        .diagnostic_data(diagnostic_data),
        .applied_toggle(applied_toggle),.actual_mode(actual_mode),.applied_seq(applied_seq),.applied_frame(applied_frame),
        .mode_toggle(mode_toggle),.desired_mode(desired_mode),.desired_seq(desired_seq),
        .clk(nclk),
        .rstn(rnrstn),
        .link(link),
        .rxd(mac_rxd),
        .rxvalid(mac_rxvalid),
        .rxerr(mac_rxerr),
        .txd(mac_txd),
        .txvalid(mac_valid),
        .txready(mac_ready),
        .empty(empty),
        .frame_id(frame_id),
        .source_id(source_id),
        .packet_id(packet_id),
        .stream_port(port),
        .release_packet(release_packet),
        .pixel_addr(addr),
        .pixel_data(pixel_data),
        .peer_known(known),
        .sent_packets(sent),
        .rx_fault(fault)
    );
endmodule
