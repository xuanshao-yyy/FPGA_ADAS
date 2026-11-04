`timescale 1ns/1ps
module tb_panorama_full #(parameter THIRD_HD1080=0);
 reg dc=0,vc=0,rstn=0;reg [3:0] pc=0;
 always #5 dc=~dc;
 always #13.469387755 vc=~vc;
 always #3.367 pc[0]=~pc[0];
 always #3.370 pc[1]=~pc[1];
 always #(THIRD_HD1080?3.365:6.735) pc[2]=~pc[2];
 always #11.9 pc[3]=~pc[3];
`ifndef SIMULATION
 GTP_GRS GRS_INST(.GRS_N(rstn));
`endif
 reg [3:0] iv=0,ih=0;reg [15:0] ip[0:3];reg [7:0] byte_in=0;
 wire cv,ch,cd;wire [15:0] cp;
 camera_pixels cam(pc[3],rstn,iv[3],ih[3],byte_in,cd,ch,cv,cp);
 wire [1031:0] q;wire [3:0] empty,pop;wire [31:0] drops[0:3];
 genvar c;
 generate for(c=0;c<4;c=c+1)begin:cap
  wire [23:0] raw={ip[c][15:11],ip[c][15:13],ip[c][10:5],ip[c][10:9],ip[c][4:0],ip[c][4:2]};
  wire [25:0] hd;
  hdmi_pixels #(.DELAY_TAPS(127)) input_stage(pc[c],rstn,iv[c],ih[c],raw,hd);
  if(c==2)begin: recovery
  scale_capture_ch2_hold #(.WIDTH(1280),.HEIGHT(720)) dut(.pclk(pc[c]),.rstn(rstn),.vs(c==3?cv:hd[25]),.line_active(c==3?ch:hd[24]),
  .valid(c==3?cd:hd[24]),.pixel(c==3?cp:{hd[23:19],hd[15:10],hd[7:3]}),.dclk(dc),.drstn(rstn),.pop(pop[c]),
  .data(q[c*258+:258]),.empty(empty[c]),.source_width(),.source_height(),.dropped(drops[c]));
  end else begin: original
  scale_capture #(.WIDTH(1280),.HEIGHT(720)) dut(.pclk(pc[c]),.rstn(rstn),.vs(c==3?cv:hd[25]),.line_active(c==3?ch:hd[24]),
  .valid(c==3?cd:hd[24]),.pixel(c==3?cp:{hd[23:19],hd[15:10],hd[7:3]}),.dclk(dc),.drstn(rstn),.pop(pop[c]),
  .data(q[c*258+:258]),.empty(empty[c]),.source_width(),.source_height(),.dropped(drops[c]));
  end
 end endgenerate
 wire [27:0] awaddr,araddr;wire [3:0] awlen,arlen;
 wire awvalid,arvalid;wire [255:0] wdata,rdata;
 reg writing=0,reading=0;
 integer wi=0,ri=0,wn=0,rn=0,cycles=0;
 reg [255:0] mem[0:786431];
 wire awready=!writing&&awvalid&&(cycles%11!=0);
 wire arready=!reading&&arvalid&&(cycles%13!=0);
 wire wready=writing&&(cycles%7!=0),rvalid=reading&&(cycles%5!=0);
 wire wlast=wn==7,rlast=rn==7;
 assign rdata=mem[ri+rn];
 wire [3:0] available;wire fault,hs,vs,de;wire [63:0] pixels;wire [127:0] source_seq;
 reg parking=0,panorama=0;wire boundary;wire [31:0] raster_frame;wire [1:0] quad_select;wire [95:0] diagnostic;
 always @(posedge vc)if(boundary)case(raster_frame)
     3:begin parking<=1;panorama<=0;end
     6:begin parking<=0;panorama<=1;end
     9:begin parking<=0;panorama<=0;end
    endcase
 full_buffer #(.PRODUCT(1)) dut(dc,rstn,vc,rstn,q,empty,pop,awaddr,awlen,awvalid,awready,wdata,wready,wlast,
 araddr,arlen,arvalid,arready,rdata,rvalid,rlast,available,fault,hs,vs,de,pixels,source_seq,parking,boundary,raster_frame,quad_select,diagnostic,panorama);
 function integer mi(input [27:0] a);
  mi=a[22:21]*196608+a[20:19]*65536+a[18:3];
 endfunction
 always @(posedge dc)begin
  cycles<=cycles+1;
  if(awvalid&&awready)begin
   if(awlen!=7||awaddr[18:3]>=57600||awaddr[20:19]>2)$fatal(1,"write address/burst");
   wi<=mi(awaddr);wn<=0;writing<=1;
  end
  if(wready)begin mem[wi+wn]<=wdata;if(wlast)writing<=0;else wn<=wn+1;end
  if(arvalid&&arready)begin
   if(arlen!=7||araddr[18:3]>=57600||araddr[20:19]>2)$fatal(1,"read address/burst");
   ri<=mi(araddr);rn<=0;reading<=1;
  end
  if(rvalid)begin
   if(^rdata===1'bx)$fatal(1,"reading unwritten memory %d",ri+rn);
   if(rlast)reading<=0;else rn<=rn+1;
  end
  if(rstn&&fault)$fatal(1,"underflow or storage fault");
 end
 generate for(c=0;c<4;c=c+1)begin:protect
  always @(posedge dc)if(awvalid&&awready&&awaddr[22:21]==c&&available[c]&&awaddr[20:19]==dut.channels[c].store.db)
   $fatal(1,"overwriting channel %d displayed bank",c);
 end endgenerate
 function [15:0] sample(input integer channel,frame,x,y);
  sample={5'(channel*5+frame+1),6'(y),5'(x)};
 endfunction
 function [23:0] expand(input [15:0] p);
  expand={p[15:11],p[15:13],p[10:5],p[10:9],p[4:0],p[4:2]};
 endfunction
 task automatic send_frame(input integer chn,f,w,h);
  integer x,y;reg [15:0] p,bgr;
  begin
   @(posedge pc[chn]);#1;iv[chn]=1;ih[chn]=0;
   repeat(12)@(posedge pc[chn]);#1;iv[chn]=0;
   repeat(20)@(posedge pc[chn]);#1;
   for(y=0;y<h;y=y+1)begin
    ih[chn]=1;
    for(x=0;x<w;x=x+1)begin
     p=sample(chn,f,x,y);ip[chn]=p;
     if(chn==3)begin
      bgr={p[4:0],p[10:5],p[15:11]};byte_in=bgr[15:8];@(posedge pc[chn]);#1;byte_in=bgr[7:0];
     end
     @(posedge pc[chn]);#1;
    end
    ih[chn]=0;
    repeat(chn==3?1224:w==1920?280:370)@(posedge pc[chn]);#1;
   end
   repeat(chn==3?75680:w==1920?98968:49468)@(posedge pc[chn]);#1;
  end
 endtask
 task automatic source(input integer chn,w,h);
  integer f;
  begin
   wait(rstn);repeat(chn*19+1)@(negedge pc[chn]);
   for(f=0;f<3;f=f+1)send_frame(chn,f,w,h);
   // Boundary commits the final frame, then source stops (freeze last frame).
   iv[chn]=1;repeat(12)@(negedge pc[chn]);iv[chn]=0;
  end
 endtask

 function [15:0] oriented(input integer chn,xx,yy);
 integer u,v,ratio,f;begin
  u=chn==1?1279-xx:xx;v=chn==1?719-yy:yy;
  ratio=chn<2||(chn==2&&THIRD_HD1080);f=source_seq[chn*32+:32];
  oriented=sample(chn,f,ratio?(((u+1)*3+1)/2-1):u,ratio?(((v+1)*3+1)/2-1):v);
 end endfunction
 function integer fw(input integer n);begin case(n)
 0:fw=0;1:fw=3;2:fw=12;3:fw=27;4:fw=45;5:fw=66;6:fw=90;7:fw=115;
 8:fw=141;9:fw=166;10:fw=190;11:fw=211;12:fw=229;13:fw=244;14:fw=253;15:fw=256;
 endcase end endfunction
 function [15:0] expected_panorama(input integer px,py);
 integer pos,a,b,ax,bx,ay,by,w,r,g,bl;reg va,vb;reg [15:0] pa,pb;begin
  pos=px-28;a=0;b=0;ax=0;bx=0;w=0;va=pos>=0&&pos<2504;vb=0;
  if(pos<464)begin a=3;ax=(pos*4+2)/3;end
  else if(pos<932)begin a=1;ax=156+pos-464;end
  else if(pos<1556)begin a=0;ax=pos-932;end
  else if(pos<2024)begin a=2;ax=pos-1556;end
  else begin a=3;ax=((pos-2024)*4+2)/3;end
  if(pos>=464&&pos<480)begin a=3;ax=(pos*4+2)/3;b=1;bx=156+pos-464;w=fw(pos-464);vb=1;end
  if(pos>=932&&pos<948)begin a=1;ax=156+pos-464;b=0;bx=pos-932;w=fw(pos-932);vb=1;end
  if(pos>=1556&&pos<1572)begin a=0;ax=pos-932;b=2;bx=pos-1556;w=fw(pos-1556);vb=1;end
  if(pos>=2024&&pos<2040)begin a=2;ax=pos-1556;b=3;bx=((pos-2024)*4+2)/3;w=fw(pos-2024);vb=1;end
  ay=a==3?((py-45)*4+2)/3:py;by=b==3?((py-45)*4+2)/3:py;
  if(a==3&&(py<45||py>=315))va=0;if(b==3&&(py<45||py>=315))vb=0;
  pa=va?oriented(a,ax*2,ay*2):0;pb=vb?oriented(b,bx*2,by*2):0;
  if(!va&&vb)w=256;else if(!vb)w=0;
  r=(pa[15:11]*(256-w)+pb[15:11]*w+128)>>8;
  g=(pa[10:5]*(256-w)+pb[10:5]*w+128)>>8;
  bl=(pa[4:0]*(256-w)+pb[4:0]*w+128)>>8;
  expected_panorama={5'(r),6'(g),5'(bl)};
 end endfunction
 integer drive_checked=0,park_checked=0,pano_checked=0;
 integer count=0,outframes=0,tile,x,y,id,i,ratio,checked=0;
 integer ids[0:3],lastids[0:3],seen[0:3],repeats=0;
 reg oldvs=0;
 always @(posedge vc)begin
  #1;
  if(rstn)begin
   if(vs&&!oldvs)begin
    if(count!=0)begin
     if(count!=1280*720)$fatal(1,"raster pixel count %d",count);
     outframes=outframes+1;
     for(i=0;i<4;i=i+1)begin
      if(ids[i]>=1)seen[i]=seen[i]+1;
      if(ids[i]>=1&&ids[i]==lastids[i])repeats=repeats+1;
      lastids[i]=ids[i];
     end
    end
    count=0;for(i=0;i<4;i=i+1)ids[i]=-2;
   end
   if(de)begin
    x=count%1280;y=count/1280;
    if(!panorama&&quad_select!={1'(y>=360),1'(x>=640)})$fatal(1,"Quad selector/pixel alignment");
    if(panorama)begin
     if(available==15)begin
      if(pixels[15:0]!==expected_panorama(x+1280*(y%2),y/2))$fatal(1,"panorama mismatch xy=%0d,%0d actual=%h expected=%h",x+1280*(y%2),y/2,pixels[15:0],expected_panorama(x+1280*(y%2),y/2));
      if(pixels[63:16]!==0)$fatal(1,"Non-primary panorama stream leaked");
      pano_checked=pano_checked+1;
     end
    end else for(tile=0;tile<4;tile=tile+1)begin
     id=pixels[tile*16+:16]==0?-1:pixels[tile*16+11+:5]-tile*5-1;
     if(ids[tile]==-2)ids[tile]=id;
     if(id!=ids[tile])$fatal(1,"torn/mixed channel %d frame %d xy %d %d",tile,outframes,x,y);
     if(id>=0)begin
      // Original scaler emits when the running ratio crosses the next pixel.
      ratio=(tile<2||(tile==2&&THIRD_HD1080));
      if(id<1||id>2||pixels[tile*16+:16]!==sample(tile,id,
          ratio?((((tile==1?1279-(parking?2*(x%640):x):(parking?2*(x%640):x))+1)*3+1)/2-1):(parking?2*(x%640):x),
          ratio?((((tile==1?719-(parking?2*(y%360):y):(parking?2*(y%360):y))+1)*3+1)/2-1):(parking?2*(y%360):y)))
       $fatal(1,"full frame mismatch channel=%d id=%d xy=%d,%d pixel=%h",tile,id,x,y,pixels[tile*16+:16]);
      checked=checked+1;if(parking)park_checked=park_checked+1;else drive_checked=drive_checked+1;
     end
    end
    count=count+1;
   end
  end
  oldvs=vs;
 end
 initial begin
  for(i=0;i<4;i=i+1)begin ip[i]=0;ids[i]=-2;lastids[i]=-2;seen[i]=0;end
  #100;rstn=1;
  fork source(0,1920,1080);source(1,1920,1080);source(2,THIRD_HD1080?1920:1280,THIRD_HD1080?1080:720);source(3,1280,720);join
  #140000000;
  for(i=0;i<4;i=i+1)if(drops[i]!=0||seen[i]<2)$fatal(1,"channel %d drops=%d displayed=%d",i,drops[i],seen[i]);
  if(outframes<4||repeats<4||available!=15)$fatal(1,"output coverage missing");
  if(drive_checked==0||park_checked==0||pano_checked==0)$fatal(1,"Mode coverage missing");
  $display("PASS: CH1 rotated, DRIVE/PARK/PANORAMA/DRIVE; four asynchronous inputs (HDMI IN1/2 1080p60, IN3 1080p=%0d, camera 720p15 bytes), exact 1280x720 full frames, DDR stalls, bank protection, four independent streams, repeats and stop/freeze; frames=%0d checked_pixels=%0d",THIRD_HD1080,outframes,checked);
  $finish;
 end
 initial begin #350000000;$fatal(1,"timeout");end
endmodule
