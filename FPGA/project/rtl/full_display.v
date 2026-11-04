`timescale 1ns / 1ps

// Four independent full-resolution pixels on every active raster cycle.
// Line caches alternate by row parity; each stream keeps a frame-stable bank.
// 在 video_clk 域产生 1650×750 总时序，其中有效画面为 1280×720。
// 四路像素并行输出；某路行标签无效时只把该路置黑，不把旧行当新行使用。
module full_display #(parameter PRODUCT=0) (
    input clk, rstn,
    input [3:0] line_ack,
    input [95:0] line_tags,
    input [1023:0] ram_data,
    output reg frame_req, line_req,
    output reg [9:0] line_row,
    output [7:0] ram_addr,
    output hs, vs, de,
    output [63:0] pixels,
    output reg underflow,
    input parking,output boundary,output reg [31:0] frame_count,output reg [1:0] quad_select,
    input panorama,input [127:0] source_seq,output [31:0] ram_addrs
);
    reg [11:0] h, v;
    reg [3:0] ack1, ack2, ack3, good, good1;
    reg [95:0] tags1, tags2;

    // h/v 是总时序位置；减去消隐区得到有效像素坐标 x/y，行缓存按 y 的奇偶交替使用。
    wire [11:0] x = h - 260, y = v - 25;
    wire [9:0] row = y[9:0];
    wire [10:0] physical_x=(PRODUCT && parking) ? ((x>=640 ? x-640 : x)<<1) : x;
    assign ram_addr = {y[0], physical_x[10:4]};
    assign boundary=(h==0 && v==0);
    reg [1:0] quad1;
    reg hs1, vs1, de1;
    reg [3:0] lane1;
    integer i;
    reg normal_hs,normal_vs,normal_de;reg [63:0] normal_pixels;
    wire pano_on=PRODUCT && panorama;
    wire pano_hs,pano_vs,pano_de;wire [15:0] pano_pixel;wire [31:0] pano_addr;
    (* ASYNC_REG="TRUE" *) reg [127:0] seq1,seq2;
    reg [31:0] last_seq[0:3];reg [4:0] source_age[0:3];reg [3:0] fresh;
    integer ch;
    always @(posedge clk or negedge rstn)begin
      if(!rstn)begin seq1<=0;seq2<=0;fresh<=0;for(ch=0;ch<4;ch=ch+1)begin last_seq[ch]<=0;source_age[ch]<=31;end end
      else begin seq1<=source_seq;seq2<=seq1;
        if(h==200&&v==20)for(ch=0;ch<4;ch=ch+1)begin
          if(seq2[ch*32+:32]!=last_seq[ch]&&seq2[ch*32+:32]!=0)begin source_age[ch]<=0;fresh[ch]<=1;end
          else if(source_age[ch]<15)source_age[ch]<=source_age[ch]+1'b1;
          else fresh[ch]<=0;
          last_seq[ch]<=seq2[ch*32+:32];
        end
      end
    end
    panorama_line strip(.clk(clk),.rstn(rstn),.x(x[10:0]),.y(y[9:0]),
      .hs(h<40),.vs(v<5),.de(h>=260&&h<1540&&v>=25&&v<745),
      .good(good&fresh),.ram_data(ram_data),.ram_addrs(pano_addr),.pixel(pano_pixel),
      .ohs(pano_hs),.ovs(pano_vs),.ode(pano_de));
    assign ram_addrs=pano_on?pano_addr:{4{ram_addr}};
    assign hs=pano_on?pano_hs:normal_hs;assign vs=pano_on?pano_vs:normal_vs;
    assign de=pano_on?pano_de:normal_de;assign pixels=pano_on?{48'd0,pano_pixel}:normal_pixels;


    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            h <= 0;frame_count<=0;quad_select<=0;quad1<=0;
            v <= 0;
            ack1 <= 0;
            ack2 <= 0;
            ack3 <= 0;
            tags1 <= 0;
            tags2 <= 0;
            good <= 0;
            good1 <= 0;
            frame_req <= 0;
            line_req <= 0;
            line_row <= 0;
            normal_hs <= 0;
            normal_vs <= 0;
            normal_de <= 0;
            normal_pixels <= 0;
            underflow <= 0;
            hs1 <= 0;
            vs1 <= 0;
            de1 <= 0;
            lane1 <= 0;
        end else begin

            // ack 同步三拍，行标签同步两拍；标签内容在对应请求完成后保持稳定。
            ack1  <= line_ack;
            ack2  <= ack1;
            ack3  <= ack2;
            tags1 <= line_tags;
            tags2 <= tags1;
            if (h == 1649) begin
                h <= 0;
                if (v == 749) v <= 0;
                else v <= v + 1'b1;
            end else h <= h + 1'b1;

            // 每帧开始翻转 frame_req；在下一行显示前发起 line_req，等待四路上一次请求均完成。
            if (h == 0 && v == 0) begin frame_req <= ~frame_req;frame_count<=frame_count+1'b1;end
            if (h == 0 && v >= 24 && v < 744) begin
                if (ack3 == {4{line_req}}) begin
                    line_row <= v - 24;
                    line_req <= ~line_req;
                end else underflow <= 1;
            end

            // 有效行前检查标签：必须同时匹配“有效”、当前帧 epoch 和目标行号。
            if (h == 256 && v >= 25 && v < 745) begin
                for (i = 0; i < 4; i = i + 1) begin
                    good[i] <= tags2[i*24+y[0]*12+:12] == {1'b1, frame_req, row};
                    if (tags2[i*24+y[0]*12+11] && tags2[i*24+y[0]*12+:12] != {1'b1, frame_req, row})
                        underflow <= 1;
                end
            end

            // 像素与 HS/VS/DE 同步流水；只有该路标签匹配时才读取当前行，否则输出黑色。
            hs1 <= h < 40;
            vs1 <= v < 5;
            de1 <= h >= 260 && h < 1540 && v >= 25 && v < 745;
            lane1 <= physical_x[3:0];
            quad1<={y>=360,x>=640};quad_select<=quad1;
            good1 <= good;
            normal_hs <= hs1;
            normal_vs <= vs1;
            normal_de <= de1;
            for (i = 0; i < 4; i = i + 1)
            normal_pixels[i*16+:16] <= de1 && good1[i] ? ram_data[i*256+lane1*16+:16] : 16'd0;
        end
    end
endmodule
