`timescale 1ns / 1ps

// Grant one ring until MAC release. Round robin changes only between packets.
// 两路网络队列的整包仲裁；stream_port = FIRST_PORT + selected。
// active 期间 selected 保持不变，MAC 完成 FCS 后才允许换通道。
module dual_packet_mux #(
    parameter PRODUCT=0,
    parameter FIRST_PORT = 5000
) (
    input clk, rstn,
    input [1:0] empty_in,
    input [63:0] frames, sources,
    input [21:0] packets,
    input [63:0] data_in,
    input release_packet,
    output [1:0] release_out,
    output empty,
    output [31:0] frame_id, source_id,
    output [10:0] packet_id,
    output [31:0] pixel_data,
    output [15:0] stream_port,
    input [3:0] composite
);
    reg active, selected, turn;

    // 优先选择 turn；若该路为空，就检查另一条队列。空队列不会占用授权。
    wire pick = (!empty_in[turn]) ? turn : !turn;
    assign empty = !active;
    assign frame_id = frames[selected*32+:32];
    assign source_id = sources[selected*32+:32];
    assign packet_id = packets[selected*11+:11];
    assign pixel_data = data_in[selected*32+:32];
    assign stream_port = PRODUCT && composite[selected*2+:2]==2 ? 5200 : PRODUCT && composite[selected*2+:2]==1 ? 5100 : FIRST_PORT + selected;
    assign release_out = active && release_packet ? (selected ? 2'b10 : 2'b01) : 2'b00;

    // release_packet 只释放已锁定的队列；更换 turn 后再选择下一包。
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            active <= 0;
            selected <= 0;
            turn <= 0;
        end else if (active) begin
            if (release_packet) begin
                active <= 0;
                turn   <= !selected;
            end
        end else if (!(&empty_in)) begin
            selected <= pick;
            active   <= 1;
        end
    end
endmodule
