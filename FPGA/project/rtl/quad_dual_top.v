`timescale 1ns / 1ps

// Board integration: HDMI IN1/2 -> SFP0; HDMI IN3/CAM1 -> SFP1.
// Capture -> per-source triple DDR buffers -> four full-resolution readers
// -> fresh-frame packet queues -> independent MAC/PCS -> HSST lanes 0 and 1.
// CAM1 uses the hardware-verified 15fps setting; HDMI readers run at ~30Hz.
// 板级顶层：IN1/IN2 共用 SFP0，IN3/CAM1 共用 SFP1；HDMI OUT 预览 IN1。
// 数据路径：输入采样 → 缩放与跨时钟 FIFO → DDR 三缓冲 → 行缓存 → 网络分包。
// sys_clk 负责初始化；各输入使用自己的 PCLK；core_clk 访问 DDR；video_clk 读图；net_clk 发包。
module quad_dual_top (
    input sfp_refp, sfp_refn, sfp_rxp, sfp_rxn,
    output sfp_txp, sfp_txn, sfp_disable,
    input sfp1_rxp, sfp1_rxn,
    output sfp1_txp, sfp1_txn, sfp1_disable,
    output sfp2_disable, // SFP2 保持发送禁止；这不切断模块 3.3V 供电。
    input sys_clk, key_n,
    inout rx_scl, rx_sda,
    input rx1_pclk, rx1_vs, rx1_de,
    input [23:0] rx1_data,
    input rx2_pclk, rx2_vs, rx2_de,
    input [23:0] rx2_data,
    input rx3_pclk, rx3_vs, rx3_de,
    input [23:0] rx3_data,
    inout cmos1_scl, cmos1_sda,
    input cmos1_vsync, cmos1_href, cmos1_pclk,
    input [7:0] cmos1_data,
    output cmos1_reset, cmos2_reset,
    output mem_rst_n, mem_ck, mem_ck_n, mem_cke, mem_cs_n, mem_ras_n, mem_cas_n, mem_we_n, mem_odt,
    output [14:0] mem_a,
    output [2:0] mem_ba,
    inout [3:0] mem_dqs, mem_dqs_n,
    inout [31:0] mem_dq,
    output [3:0] mem_dm,
    inout tx_scl, tx_sda,
    output tx_reset_n, rx_reset_n, rx3_reset_n,
    output tx_pclk,
    output reg tx_vs, tx_hs, tx_de,
    output reg [23:0] tx_data,
    output [7:0] led
);

    // 时钟与上电复位。50MHz sys_clk 下，5000000/10000000 周期对应约 100/200ms。
    wire video_clk, locked, core_clk, ddr_init_done, pll_lock;
    wire cfg_clk;
    GTP_CLKBUFG init_clock_buffer(.CLKIN(sys_clk),.CLKOUT(cfg_clk));
    wire cfg_rstn, drstn, crstn, vrstn;
    reg [23:0] power_count = 0;
    reg [25:0] heartbeat = 0;
    wire rstn_out = (power_count >= 5000000) && key_n && locked;
    wire init_reset = rstn_out && (power_count >= 10000000);
    dual_reset sys_reset (
        cfg_clk,
        key_n && locked,
        cfg_rstn
    );

    always @(posedge cfg_clk) begin
        if (!cfg_rstn) begin
            power_count <= 0;
            heartbeat   <= 0;
        end else begin
            if (power_count < 15000000) power_count <= power_count + 1'b1;
            heartbeat <= heartbeat + 1'b1;
        end
    end
    assign tx_reset_n  = rstn_out;
    assign rx_reset_n  = rstn_out;
    assign rx3_reset_n = rstn_out;
    assign cmos1_reset = rstn_out;
    assign cmos2_reset = 0;
    video_pll u_pll (
        .clkin1  (sys_clk),
        .clkout0 (video_clk),
        .clkout1 (),
        .clkout2 (),
        .pll_lock(locked)
    );
    hdmi_forward_clock forward (
        video_clk,
        tx_pclk
    );

    // 外设初始化：IN1/IN2 共用 rx_scl/rx_sda；IN3/HDMI OUT 共用 tx_scl/tx_sda。
    wire cam_ready, cam_fault, cam_id, tx_ready, tx_fault;
    wire [2:0] rx_ready, rx_fault;
    wire rx3_probe_ok,rx3_table_done,rx3_poll_valid;wire [7:0] rx3_poll_data;
    ov5640_init sensor (
        .clk(cfg_clk),
        .rstn(rstn_out),
        .enable(power_count >= 10000000),
        .scl(cmos1_scl),
        .sda(cmos1_sda),
        .ready(cam_ready),
        .fault(cam_fault),
        .id_ok(cam_id),
        .index(),
        .test_pattern(1'b0),
        .slow_camera(1'b1)
    );
    ms72xx_pair_init #(
        .SECOND_RX  (1),
        .FIRST_ADDR (7'h59),
        .SECOND_ADDR(7'h2b)
    ) pair12 (
        .clk(cfg_clk),
        .rst_n(init_reset),
        .scl(rx_scl),
        .sda(rx_sda),
        .rx_ready(rx_ready[0]),
        .tx_ready(rx_ready[1]),
        .rx_fault(rx_fault[0]),
        .tx_fault(rx_fault[1]),
        .probe_ok(),
        .table_done(),
        .poll_valid(),
        .poll_data()
    );
    ms72xx_pair_init pair3out (
        .clk(cfg_clk),
        .rst_n(init_reset),
        .scl(tx_scl),
        .sda(tx_sda),
        .rx_ready(rx_ready[2]),
        .tx_ready(tx_ready),
        .rx_fault(rx_fault[2]),
        .tx_fault(tx_fault),
        .probe_ok(rx3_probe_ok),
        .table_done(rx3_table_done),
        .poll_valid(rx3_poll_valid),
        .poll_data(rx3_poll_data)
    );

    // DDR 就绪后统一释放采集/缓存复位；各时钟域分别同步释放，不能只重置 FIFO 一端。
    dual_reset dreset (
        core_clk,
        rstn_out && ddr_init_done,
        drstn
    );
    dual_reset creset (
        cmos1_pclk,
        drstn && cam_ready,
        crstn
    );

    // Always output a raster after TX/DDR setup, even when sources are absent.
    dual_reset vreset (
        video_clk,
        drstn,
        vrstn
    );

    // q 按通道顺序拼接四个 258-bit FIFO 字：CH0 IN1、CH1 IN2、CH2 IN3、CH3 CAM1。
    wire [1031:0] q;
    wire [3:0] empty, pop;
    wire cv, ch, cd;
    wire [15:0] cp;
    camera_pixels camera (
        .pclk(cmos1_pclk),
        .rstn(crstn),
        .vs(cmos1_vsync),
        .href(cmos1_href),
        .din(cmos1_data),
        .de(cd),
        .line_active(ch),
        .vs_d(cv),
        .pixel(cp)
    );
    scale_capture #(
        .WIDTH (1280),
        .HEIGHT(720)
    ) cap_cam (
        .pclk(cmos1_pclk),
        .rstn(crstn),
        .vs(cv),
        .line_active(ch),
        .valid(cd),
        .pixel(cp),
        .dclk(core_clk),
        .drstn(drstn),
        .pop(pop[3]),
        .data(q[3*258+:258]),
        .empty(empty[3]),
        .source_width(),
        .source_height(),
        .dropped()
    );

    // 三个 HDMI 输入保持相同采样和缩放结构；RGB888 在入缓存前取位转换为 RGB565。
    wire rr2,rr3;wire [1:0] net_clk;
    wire [255:0] capture1_probe,capture3_probe,capture1_snap,capture3_snap;
    wire [95:0] store_probe,store_snap;
    wire [95:0] capture3_bad,capture3_bad_snap;
    wire [31:0] receiver_snap;

    wire [31:0] receiver_probe={16'd0,rx3_probe_ok,rx3_table_done,rx3_poll_valid,rx_fault[2],rx_ready[2],tx_fault,tx_ready,1'b0,rx3_poll_data};
    wire [767:0] diagnostic_data={32'h51443031,32'd2,capture3_snap,store_snap,capture1_snap[255:32],capture3_bad_snap,receiver_snap};
    diag_snapshot #(.WIDTH(96)) probe_bad(rx3_pclk,rr3,capture3_bad,net_clk[0],drstn,capture3_bad_snap);
    diag_snapshot #(.WIDTH(32)) probe_receiver(cfg_clk,init_reset,receiver_probe,net_clk[0],drstn,receiver_snap);
    diag_snapshot #(.WIDTH(256)) probe1(rx2_pclk,rr2,capture1_probe,net_clk[0],drstn,capture1_snap);
    diag_snapshot #(.WIDTH(256)) probe3(rx3_pclk,rr3,capture3_probe,net_clk[0],drstn,capture3_snap);
    diag_snapshot #(.WIDTH(96)) probe_store(core_clk,drstn,store_probe,net_clk[0],drstn,store_snap);
    wire rr1, rv1, rd1;
    wire [23:0] rgb1;
    hdmi_pixels #(
        .DELAY_TAPS(127)
    ) input1 (
        rx1_pclk,
        rr1,
        rx1_vs,
        rx1_de,
        rx1_data,
        {rv1, rd1, rgb1}
    );

    // FIFO pointers share global DDR reset; format/lock changes are discarded by
    // the measured-frame scaler, without resetting only one side of the FIFO.
    dual_reset rxreset1 (
        rx1_pclk,
        drstn,
        rr1
    );
    scale_capture #(
        .WIDTH (1280),
        .HEIGHT(720)
    ) cap1 (
        .pclk(rx1_pclk),
        .rstn(rr1),
        .vs(rv1),
        .line_active(rd1),
        .valid(rd1),
        .pixel({rgb1[23:19], rgb1[15:10], rgb1[7:3]}),
        .dclk(core_clk),
        .drstn(drstn),
        .pop(pop[0]),
        .data(q[0*258+:258]),
        .empty(empty[0]),
        .source_width(),
        .source_height(),
        .dropped()
    );
    wire rv2, rd2;
    wire [23:0] rgb2;
    hdmi_pixels #(
        .DELAY_TAPS(127)
    ) input2 (
        rx2_pclk,
        rr2,
        rx2_vs,
        rx2_de,
        rx2_data,
        {rv2, rd2, rgb2}
    );

    // FIFO pointers share global DDR reset; format/lock changes are discarded by
    // the measured-frame scaler, without resetting only one side of the FIFO.
    dual_reset rxreset2 (
        rx2_pclk,
        drstn,
        rr2
    );
    scale_capture #(
        .WIDTH (1280),
        .HEIGHT(720),.DIAGNOSTICS(1)
    ) cap2 (
        .diagnostic(capture1_probe),
        .pclk(rx2_pclk),
        .rstn(rr2),
        .vs(rv2),
        .line_active(rd2),
        .valid(rd2),
        .pixel({rgb2[23:19], rgb2[15:10], rgb2[7:3]}),
        .dclk(core_clk),
        .drstn(drstn),
        .pop(pop[1]),
        .data(q[1*258+:258]),
        .empty(empty[1]),
        .source_width(),
        .source_height(),
        .dropped()
    );
    wire rv3, rd3;
    wire [23:0] rgb3;
    hdmi_pixels #(
        .DELAY_TAPS(127)
    ) input3 (
        rx3_pclk,
        rr3,
        rx3_vs,
        rx3_de,
        rx3_data,
        {rv3, rd3, rgb3}
    );

    // FIFO pointers share global DDR reset; format/lock changes are discarded by
    // the measured-frame scaler, without resetting only one side of the FIFO.
    dual_reset rxreset3 (
        rx3_pclk,
        drstn,
        rr3
    );
    scale_capture_ch2_hold #(
        .WIDTH (1280),
        .HEIGHT(720),.DIAGNOSTICS(1)
    ) cap3 (
        .diagnostic_bad(capture3_bad),
        .diagnostic(capture3_probe),
        .pclk(rx3_pclk),
        .rstn(rr3),
        .vs(rv3),
        .line_active(rd3),
        .valid(rd3),
        .pixel({rgb3[23:19], rgb3[15:10], rgb3[7:3]}),
        .dclk(core_clk),
        .drstn(drstn),
        .pop(pop[2]),
        .data(q[2*258+:258]),
        .empty(empty[2]),
        .source_width(),
        .source_height(),
        .dropped()
    );

    // DDR 数据接口。控制器每拍提供 256 bit，四路读写由 full_buffer 内部轮询仲裁。
    wire [27:0] axi_awaddr, axi_araddr;
    wire [3:0] axi_awlen, axi_arlen;
    wire axi_awvalid, axi_arvalid, axi_awready, axi_arready, axi_wready, axi_wusero_last,
        axi_rvalid, axi_rlast;
    wire [255:0] axi_wdata, axi_rdata;
    wire [31:0] axi_wstrb = 32'hffffffff;
    wire [3:0] axi_awuser_id = 0, axi_aruser_id = 0;
    wire [3:0] axi_rid;
    wire hs, vs, de, fault;
    wire [63:0] pixels;
    wire boundary,mode_toggle,applied_toggle;wire [1:0] desired_mode,display_mode;
    wire parking=display_mode==1,panorama=display_mode==2;
    wire [31:0] raster_frame,desired_seq,applied_seq,applied_frame;
    wire [1:0] quad_select;
    wire [15:0] quad_pixel=pixels[quad_select*16+:16];
    mode_bridge park_bridge(.vclk(video_clk),.rstn(vrstn),.boundary(boundary),.frame_count(raster_frame),
      .request_toggle(mode_toggle),.desired_mode(desired_mode),.desired_seq(desired_seq),
      .applied_toggle(applied_toggle),.parking(display_mode),.applied_seq(applied_seq),.applied_frame(applied_frame));
    wire [127:0] source_seq;

    // HDMI OUT 仅取 CH0 的像素；RGB565 扩展为 RGB888 时复制高位补足低位。
    wire [15:0] monitor = panorama ? 16'h2104 : parking ? quad_pixel : pixels[15:0];
    wire [23:0] rgb = {
        monitor[15:11], monitor[15:13], monitor[10:5], monitor[10:9], monitor[4:0], monitor[4:2]
    };
    wire [3:0] available;
    full_buffer #(.PRODUCT(1)) video (
        .diagnostic_ch2(store_probe),
        .panorama(panorama),.parking(parking),.boundary(boundary),.frame_count(raster_frame),.quad_select(quad_select),
        .clk(core_clk),
        .rstn(drstn),
        .vclk(video_clk),
        .vrstn(vrstn),
        .q(q),
        .empty(empty),
        .pop(pop),
        .awaddr(axi_awaddr),
        .awlen(axi_awlen),
        .awvalid(axi_awvalid),
        .awready(axi_awready),
        .wdata(axi_wdata),
        .wready(axi_wready),
        .wlast(axi_wusero_last),
        .araddr(axi_araddr),
        .arlen(axi_arlen),
        .arvalid(axi_arvalid),
        .arready(axi_arready),
        .rdata(axi_rdata),
        .rvalid(axi_rvalid),
        .rlast(axi_rlast),
        .available(available),
        .fault(fault),
        .hs(hs),
        .vs(vs),
        .de(de),
        .pixels(pixels),
        .source_seq(source_seq)
    );

    always @(posedge video_clk) begin
        tx_hs   <= hs;
        tx_vs   <= vs;
        tx_de   <= de;
        tx_data <= rgb;
    end

    // 网络物理层：使用 HSST lane 0 和 lane 1，对应两个独立的千兆链路。
    wire hsst_pll;
    wire [1:0] tx_done, rx_done, synced, txk, rxk, disp, decerr, disperr;
    wire [15:0] txd, rxd;
    wire [1:0] net_link, peer_known, net_fault;
    wire [63:0] sent;
    assign sfp_disable  = 0;
    assign sfp1_disable = 1'b0;
    assign sfp2_disable = 1'b1;
    sfp_hsst hsst (
        .i_free_clk(cfg_clk),
        .i_pll_rst_0(!cfg_rstn),
        .i_wtchdg_clr_0(1'b0),
        .i_p_refckp_0(sfp_refp),
        .i_p_refckn_0(sfp_refn),
        .o_p_pll_lock_0(hsst_pll),
        .o_txlane_done_0(tx_done[0]),
        .o_rxlane_done_0(rx_done[0]),
        .o_p_clk2core_tx_0(net_clk[0]),
        .i_p_tx0_clk_fr_core(net_clk[0]),
        .i_p_rx0_clk_fr_core(net_clk[0]),
        .o_p_pcs_lsm_synced_0(synced[0]),
        .i_p_l0rxp(sfp_rxp),
        .i_p_l0rxn(sfp_rxn),
        .o_p_l0txp(sfp_txp),
        .o_p_l0txn(sfp_txn),
        .i_txd_0(txd[0+:8]),
        .i_txk_0(txk[0]),
        .i_tdispsel_0(disp[0]),
        .i_tdispctrl_0(1'b0),
        .o_rxd_0(rxd[0+:8]),
        .o_rxk_0(rxk[0]),
        .o_rdisper_0(disperr[0]),
        .o_rdecer_0(decerr[0]),
        .o_txlane_done_1(tx_done[1]),
        .o_rxlane_done_1(rx_done[1]),
        .o_p_clk2core_tx_1(net_clk[1]),
        .i_p_tx1_clk_fr_core(net_clk[1]),
        .i_p_rx1_clk_fr_core(net_clk[1]),
        .o_p_pcs_lsm_synced_1(synced[1]),
        .i_p_l1rxp(sfp1_rxp),
        .i_p_l1rxn(sfp1_rxn),
        .o_p_l1txp(sfp1_txp),
        .o_p_l1txn(sfp1_txn),
        .i_txd_1(txd[8+:8]),
        .i_txk_1(txk[1]),
        .i_tdispsel_1(disp[1]),
        .i_tdispctrl_1(1'b0),
        .o_rxd_1(rxd[8+:8]),
        .o_rxk_1(rxk[1]),
        .o_rdisper_1(disperr[1]),
        .o_rdecer_1(decerr[1])
    );

    // SFP0：192.168.10.2 → 192.168.10.1，CH0/CH1 分别使用 UDP 5000/5001。
    network_port #(
        .PRODUCT(1),.CONTROL(1),.PRIMARY(1),
        .LOCAL_MAC(48'h025000000a02),
        .LOCAL_IP(32'hc0a80a02),
        .PEER_IP(32'hc0a80a01),
        .FIRST_PORT(5000),
        .IMAGE_BYTES(8192),
        .SLOT_BITS(1)
    ) net0 (
        .diagnostic_data(diagnostic_data),
        .panorama(panorama),.parking(parking),.quad_pixel(quad_pixel),.raster_frame(raster_frame),
        .applied_toggle(applied_toggle),.actual_mode(display_mode),.applied_seq(applied_seq),.applied_frame(applied_frame),
        .mode_toggle(mode_toggle),.desired_mode(desired_mode),.desired_seq(desired_seq),
        .nclk(net_clk[0]),
        .arstn(cfg_rstn && hsst_pll && tx_done[0]),
        .synced(synced[0] && rx_done[0]),
        .rxd(rxd[0+:8]),
        .rxk(rxk[0]),
        .rxerr(disperr[0] || decerr[0]),
        .txd(txd[0+:8]),
        .txk(txk[0]),
        .disparity(disp[0]),
        .vclk(video_clk),
        .vrstn(vrstn),
        .vs(vs),
        .de(de),
        .pixels(pixels[0+:32]),
        .source_seq(source_seq[0+:64]),
        .link(net_link[0]),
        .known(peer_known[0]),
        .fault(net_fault[0]),
        .sent(sent[0+:32])
    );

    // SFP1：192.168.20.2 → 192.168.20.1，CH2/CH3 分别使用 UDP 5002/5003。
    network_port #(
        .PRODUCT(1),
        .LOCAL_MAC(48'h025000001402),
        .LOCAL_IP(32'hc0a81402),
        .PEER_IP(32'hc0a81401),
        .FIRST_PORT(5002),
        .IMAGE_BYTES(8192),
        .SLOT_BITS(1)
    ) net1 (
        .diagnostic_data(768'd0),
        .panorama(panorama),.parking(parking),.quad_pixel(quad_pixel),.raster_frame(raster_frame),
        .applied_toggle(1'b0),.actual_mode(1'b0),.applied_seq(32'd0),.applied_frame(32'd0),
        .mode_toggle(),.desired_mode(),.desired_seq(),
        .nclk(net_clk[1]),
        .arstn(cfg_rstn && hsst_pll && tx_done[1]),
        .synced(synced[1] && rx_done[1]),
        .rxd(rxd[8+:8]),
        .rxk(rxk[1]),
        .rxerr(disperr[1] || decerr[1]),
        .txd(txd[8+:8]),
        .txk(txk[1]),
        .disparity(disp[1]),
        .vclk(video_clk),
        .vrstn(vrstn),
        .vs(vs),
        .de(de),
        .pixels(pixels[32+:32]),
        .source_seq(source_seq[64+:64]),
        .link(net_link[1]),
        .known(peer_known[1]),
        .fault(net_fault[1]),
        .sent(sent[32+:32])
    );

    // led[7:0]：故障汇总、SFP1 对端已知/链路、SFP0 对端已知/链路、CAM 就绪、DDR 就绪、心跳。
    assign led = {
        fault || cam_fault || tx_fault || (|rx_fault) || (|net_fault),
        peer_known[1],
        net_link[1],
        peer_known[0],
        net_link[0],
        cam_ready,
        ddr_init_done,
        heartbeat[24]
    };

    // 厂商 DDR3 控制器/PHY 接口；原有端口、地址单位和固定配置全部保留。
    DDR3_50H u_DDR3_50H (
        .ref_clk                (sys_clk),
        .resetn                 (rstn_out),
        .ddr_init_done          (ddr_init_done),
        .ddrphy_clkin           (core_clk),
        .pll_lock               (pll_lock),
        .axi_awaddr             (axi_awaddr),
        .axi_awuser_ap          (1'b0),
        .axi_awuser_id          (axi_awuser_id),
        .axi_awlen              (axi_awlen),
        .axi_awready            (axi_awready),
        .axi_awvalid            (axi_awvalid),
        .axi_wdata              (axi_wdata),
        .axi_wstrb              (axi_wstrb),
        .axi_wready             (axi_wready),
        .axi_wusero_id          (),
        .axi_wusero_last        (axi_wusero_last),
        .axi_araddr             (axi_araddr),
        .axi_aruser_ap          (1'b0),
        .axi_aruser_id          (axi_aruser_id),
        .axi_arlen              (axi_arlen),
        .axi_arready            (axi_arready),
        .axi_arvalid            (axi_arvalid),
        .axi_rdata              (axi_rdata),
        .axi_rid                (axi_rid),
        .axi_rlast              (axi_rlast),
        .axi_rvalid             (axi_rvalid),
        .apb_clk                (1'b0),
        .apb_rst_n              (1'b1),
        .apb_sel                (1'b0),
        .apb_enable             (1'b0),
        .apb_addr               (8'b0),
        .apb_write              (1'b0),
        .apb_ready              (),
        .apb_wdata              (16'b0),
        .apb_rdata              (),
        .apb_int                (),
        .mem_rst_n              (mem_rst_n),
        .mem_ck                 (mem_ck),
        .mem_ck_n               (mem_ck_n),
        .mem_cke                (mem_cke),
        .mem_cs_n               (mem_cs_n),
        .mem_ras_n              (mem_ras_n),
        .mem_cas_n              (mem_cas_n),
        .mem_we_n               (mem_we_n),
        .mem_odt                (mem_odt),
        .mem_a                  (mem_a),
        .mem_ba                 (mem_ba),
        .mem_dqs                (mem_dqs),
        .mem_dqs_n              (mem_dqs_n),
        .mem_dq                 (mem_dq),
        .mem_dm                 (mem_dm),
        .debug_data             (),
        .debug_slice_state      (),
        .debug_calib_ctrl       (),
        .ck_dly_set_bin         (),
        .force_ck_dly_en        (1'b0),
        .force_ck_dly_set_bin   (8'h05),
        .dll_step               (),
        .dll_lock               (),
        .init_read_clk_ctrl     (2'b0),
        .init_slip_step         (4'b0),
        .force_read_clk_ctrl    (1'b0),
        .ddrphy_gate_update_en  (1'b0),
        .update_com_val_err_flag(),
        .rd_fake_stop           (1'b0)
    );
endmodule
