`timescale 1ns / 1ps
// Video clock -> network clock packet queue.
//
// IMAGE_BYTES must divide the 1280*720*2 byte frame exactly.
// Two pixels occupy one 32-bit RAM word, transmitted low byte first.
// A Gray-coded pointer publishes a slot only after the whole packet arrives.
// Capture never stalls. On congestion, the entire affected packet is dropped;
// its missing packet index lets the receiver reject the incomplete frame.
// 从 video_clk 到 nclk 的整包队列；只有全部像素写完才通过 Gray 指针发布。
// R3 顶层使用 8192 字节图像块、两个槽位；默认参数仍兼容原来的 1440 字节模式。
module video_packet_ring #(
    parameter PRODUCT = 0,
    parameter IMAGE_BYTES = 1440,
    parameter SLOT_BITS   = 3,
    parameter WORD_BITS   = $clog2(IMAGE_BYTES / 4)
) (
    input vclk,
    input vrstn,
    input vs,
    input de,
    input [15:0] pixel,
    input [31:0] source_seq,
    input nclk,
    input nrstn,
    input release_packet,
    input [WORD_BITS-1:0] pixel_addr,
    output [31:0] pixel_data,
    output [31:0] source_id,
    output [31:0] frame_id,
    output [10:0] packet_id,
    output empty,
    output reg overflow,
    input [1:0] composite,output [1:0] composite_out
);

    // R3 的 8192 字节块对应 4096 个 RGB565 像素，整帧恰好分为 225 包。
    localparam PIXELS_PER_PACKET = IMAGE_BYTES / 2;
    localparam PIXEL_BITS = $clog2(PIXELS_PER_PACKET);
    localparam [SLOT_BITS:0] FULL_MASK = 3 << (SLOT_BITS - 1);

    // One wrap bit. Test the CURRENT pointer to use all available slots:
    // a packet being filled owns its slot until publication at its last pixel.
    // Jumbo uses two 8KiB slots, preserving the original 16KiB RAM per stream.
    reg [SLOT_BITS:0] write_pointer, write_gray, read_pointer, read_gray;
    (* ASYNC_REG="TRUE" *) reg [SLOT_BITS:0] read_gray_meta, read_gray_sync;
    (* ASYNC_REG="TRUE" *) reg [SLOT_BITS:0] write_gray_meta, write_gray_sync;
    wire [SLOT_BITS:0] next_write = write_pointer + 1'b1;
    wire [SLOT_BITS:0] next_read = read_pointer + 1'b1;
    wire [SLOT_BITS:0] next_write_gray = (next_write >> 1) ^ next_write;
    wire full = write_gray == (read_gray_sync ^ FULL_MASK);

    // output_frame 是输出时序帧号；frame_source 是实际输入帧序号，二者不保证逐帧一一对应。
    reg vs_delayed, started, first_active_pixel;
    reg [1:0] frame_composite;
    reg frame_enabled, dropping_packet;
    reg [PIXEL_BITS-1:0] pixel_index;
    reg [10:0] packet_index;
    reg [31:0] output_frame, last_source, frame_source;
    reg [15:0] first_pixel;

    // Frame selection in DDR finishes during vertical blanking. Therefore
    // decide freshness on the first active pixel, not on the VS edge.
    // A stopped or 15fps input is not retransmitted at the 30Hz output rate.
    wire new_source = source_seq != 0 && source_seq != last_source;
    wire send_frame = first_active_pixel ? new_source : frame_enabled;

    // 一个包只在首像素检查满状态；若当时满则丢弃整包，不能中途写入半包。
    wire drop_now = pixel_index == 0 ? full : dropping_packet;
    wire accept_pixel = started && de && send_frame && !drop_now;
    wire commit_packet = accept_pixel && pixel_index == PIXELS_PER_PACKET - 1;
    wire take_packet = release_packet && !empty;
    wire [SLOT_BITS-1:0]
        metadata_slot = take_packet ? next_read[SLOT_BITS-1:0] : read_pointer[SLOT_BITS-1:0];

    assign empty = read_gray == write_gray_sync;

    always @(posedge vclk) begin
        if (accept_pixel && !pixel_index[0]) first_pixel <= pixel;
    end

    // Word byte order: pixel0 low, pixel0 high, pixel1 low, pixel1 high.
    dual_ram #(
        .W(32),
        .A(SLOT_BITS + WORD_BITS)
    ) packet_memory (
        .wc(vclk),
        .we(accept_pixel && pixel_index[0]),
        .wa({write_pointer[SLOT_BITS-1:0], pixel_index[PIXEL_BITS-1:1]}),
        .wd({pixel, first_pixel}),
        .rc(nclk),
        .ra({read_pointer[SLOT_BITS-1:0], pixel_addr}),
        .rd(pixel_data)
    );
    dual_ram #(
        .W(77),
        .A(9)
    ) metadata_memory (
        .wc(vclk),
        .we(commit_packet),
        .wa({{(9 - SLOT_BITS) {1'b0}}, write_pointer[SLOT_BITS-1:0]}),
        .wd({frame_composite, frame_source, output_frame, packet_index}),
        .rc(nclk),
        .ra({{(9 - SLOT_BITS) {1'b0}}, metadata_slot}),
        .rd({composite_out, source_id, frame_id, packet_id})
    );

    always @(posedge vclk or negedge vrstn) begin
        if (!vrstn) begin
            write_pointer <= 0;
            write_gray <= 0;
            read_gray_meta <= 0;
            read_gray_sync <= 0;
            vs_delayed <= 0;
            started <= 0;frame_composite<=0;
            first_active_pixel <= 1;
            frame_enabled <= 0;
            dropping_packet <= 0;
            pixel_index <= 0;
            packet_index <= 0;
            output_frame <= 0;
            last_source <= 0;
            frame_source <= 0;
            overflow <= 0;
        end else begin
            read_gray_meta <= read_gray;
            read_gray_sync <= read_gray_meta;
            vs_delayed <= vs;

            // VS 开启新的输出帧；是否发送要等首个有效像素，以确保 DDR 已选好本帧 source_seq。
            if (vs && !vs_delayed) begin
                started <= 1;
                first_active_pixel <= 1;
                frame_enabled <= 0;
                dropping_packet <= 0;
                pixel_index <= 0;
                packet_index <= 0;
                output_frame <= output_frame + 1'b1;
            end else if (started && de) begin
                if (first_active_pixel) begin
                    first_active_pixel <= 0;frame_composite<=PRODUCT ? composite : 1'b0;
                    frame_enabled <= new_source;
                    frame_source <= source_seq;
                    if (new_source) last_source <= source_seq;
                end
                if (send_frame) begin
                    if (pixel_index == 0) begin
                        dropping_packet <= full;
                        if (full) overflow <= 1;
                    end
                    if (pixel_index == PIXELS_PER_PACKET - 1) begin
                        pixel_index  <= 0;
                        packet_index <= packet_index + 1'b1;
                        if (!drop_now) begin
                            write_pointer <= next_write;
                            write_gray <= next_write_gray;
                        end
                    end else pixel_index <= pixel_index + 1'b1;
                end
            end
        end
    end

    // 网络端只在 MAC 完成整包后推进读指针，再以 Gray 编码反馈给视频写入端。
    always @(posedge nclk or negedge nrstn) begin
        if (!nrstn) begin
            read_pointer <= 0;
            read_gray <= 0;
            write_gray_meta <= 0;
            write_gray_sync <= 0;
        end else begin
            write_gray_meta <= write_gray;
            write_gray_sync <= write_gray_meta;
            if (take_packet) begin
                read_pointer <= next_read;
                read_gray <= (next_read >> 1) ^ next_read;
            end
        end
    end
endmodule
