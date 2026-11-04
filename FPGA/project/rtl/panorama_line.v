`timescale 1ns/1ps
// Serialized 2560x360 panorama inside the existing 1280x720 transport raster.
// Two successive transport rows form one panorama row; no extra frame buffer.
// CH0 front, CH1 left (already rotated by the shared store), CH2 right, CH3 rear.
module panorama_line(
 input clk,rstn,input [10:0] x,input [9:0] y,
 input hs,vs,de,input [3:0] good,input [1023:0] ram_data,
 output reg [31:0] ram_addrs,output reg [15:0] pixel,
 output reg ohs,ovs,ode
);
 wire [11:0] px=(y[0]?12'd1280:12'd0)+{1'b0,x}-12'd28;
 wire [9:0] py={1'b0,y[9:1]};
 reg [1:0] ca,cb;reg [9:0] xa,xb;reg va,vb;reg [8:0] weight;
 reg [10:0] sx[0:3];integer c;
 function [8:0] feather;input [3:0] n;begin case(n)
 0:feather=0;1:feather=3;2:feather=12;3:feather=27;
 4:feather=45;5:feather=66;6:feather=90;7:feather=115;
 8:feather=141;9:feather=166;10:feather=190;11:feather=211;
 12:feather=229;13:feather=244;14:feather=253;15:feather=256;
 endcase end endfunction
 function [15:0] mix565;input [15:0] a,b;input [8:0] w;
 reg [14:0] rr,bb;reg [15:0] gg;begin
 rr=a[15:11]*(9'd256-w)+b[15:11]*w+128;
 gg=a[10:5]*(9'd256-w)+b[10:5]*w+128;
 bb=a[4:0]*(9'd256-w)+b[4:0]*w+128;
 mix565={rr[12:8],gg[13:8],bb[12:8]};end endfunction
 always @*begin
 ca=0;cb=0;xa=0;xb=0;va=0;vb=0;weight=0;
 if(de&&px<2504)begin
  va=1;
  if(px<464)begin ca=3;xa=(px*4+2)/3;end
  else if(px<932)begin ca=1;xa=156+px-464;end
  else if(px<1556)begin ca=0;xa=px-932;end
  else if(px<2024)begin ca=2;xa=px-1556;end
  else begin ca=3;xa=((px-2024)*4+2)/3;end
  if(px>=464&&px<480)begin ca=3;xa=(px*4+2)/3;cb=1;xb=156+px-464;weight=feather(px-464);vb=1;end
  if(px>=932&&px<948)begin ca=1;xa=156+px-464;cb=0;xb=px-932;weight=feather(px-932);vb=1;end
  if(px>=1556&&px<1572)begin ca=0;xa=px-932;cb=2;xb=px-1556;weight=feather(px-1556);vb=1;end
  if(px>=2024&&px<2040)begin ca=2;xa=px-1556;cb=3;xb=((px-2024)*4+2)/3;weight=feather(px-2024);vb=1;end
  if(ca==3&&!(py>=45&&py<315))va=0;
  if(cb==3&&!(py>=45&&py<315))vb=0;
  if(!good[ca])va=0;if(!good[cb])vb=0;
 end
 for(c=0;c<4;c=c+1)begin
  sx[c]=0;if(ca==c)sx[c]=xa<<1;if(cb==c&&vb)sx[c]=xb<<1;
  ram_addrs[c*8+:8]={y[0],sx[c][10:4]};
 end
 end
 reg [1:0] ca1,cb1;reg [3:0] la1,lb1;reg va1,vb1;
 reg [8:0] w1,w2;reg hs1,vs1,de1,hs2,vs2,de2;
 reg [15:0] pa2,pb2;
 wire [255:0] wa=ram_data[ca1*256+:256],wb=ram_data[cb1*256+:256];
 always @(posedge clk or negedge rstn)begin
 if(!rstn)begin ca1<=0;cb1<=0;la1<=0;lb1<=0;va1<=0;vb1<=0;w1<=0;w2<=0;
 hs1<=0;vs1<=0;de1<=0;hs2<=0;vs2<=0;de2<=0;ohs<=0;ovs<=0;ode<=0;pa2<=0;pb2<=0;pixel<=0;end
 else begin
 ca1<=ca;cb1<=cb;la1<={xa[2:0],1'b0};lb1<={xb[2:0],1'b0};va1<=va;vb1<=vb;
 w1<=(!va&&vb)?9'd256:(!vb?9'd0:weight);
 hs1<=hs;vs1<=vs;de1<=de;hs2<=hs1;vs2<=vs1;de2<=de1;w2<=w1;
 pa2<=va1?wa[la1*16+:16]:16'd0;pb2<=vb1?wb[lb1*16+:16]:16'd0;
 ohs<=hs2;ovs<=vs2;ode<=de2;pixel<=de2?mix565(pa2,pb2,w2):16'd0;
 end end
endmodule
