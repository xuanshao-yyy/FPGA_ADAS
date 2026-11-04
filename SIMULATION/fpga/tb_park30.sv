`timescale 1ns/1ps
module tb_park30;
 reg nc=0,vc=0,rstn=0,vs=0,de=0,parking=1,panorama=0;
 reg [31:0] raster=1;always #4 nc=~nc;always #13.469 vc=~vc;
 wire [7:0] tx0,tx1;wire k0,k1,d0,d1,l0,l1,q0,q1,f0,f1,t0,t1;
 wire [31:0] sent0,sent1,s0,s1;wire [1:0] m0,m1;
 wire [1:0] mode=panorama?2:parking?1:0;
 network_port #(.PRODUCT(1),.PRIMARY(1),.IMAGE_BYTES(8192),.SLOT_BITS(2)) net0(
 .nclk(nc),.arstn(rstn),.synced(1'b1),.rxd(8'hbc),.rxk(1'b1),.rxerr(1'b0),.txd(tx0),.txk(k0),.disparity(d0),
 .vclk(vc),.vrstn(rstn),.vs(vs),.de(de),.pixels(32'h24681357),.source_seq({raster,raster}),.link(l0),.known(q0),.fault(f0),.sent(sent0),
 .diagnostic_data(768'd0),.parking(parking),.panorama(panorama),.quad_pixel(16'h1357),.raster_frame(raster),
 .applied_toggle(1'b0),.actual_mode(mode),.applied_seq(32'd0),.applied_frame(32'd0),.mode_toggle(t0),.desired_mode(m0),.desired_seq(s0));
 network_port #(.PRODUCT(1),.PRIMARY(0),.FIRST_PORT(5002),.IMAGE_BYTES(8192),.SLOT_BITS(2)) net1(
 .nclk(nc),.arstn(rstn),.synced(1'b1),.rxd(8'hbc),.rxk(1'b1),.rxerr(1'b0),.txd(tx1),.txk(k1),.disparity(d1),
 .vclk(vc),.vrstn(rstn),.vs(vs),.de(de),.pixels(32'h24681357),.source_seq({raster,raster}),.link(l1),.known(q1),.fault(f1),.sent(sent1),
 .diagnostic_data(768'd0),.parking(parking),.panorama(panorama),.quad_pixel(16'h1357),.raster_frame(raster),
 .applied_toggle(1'b0),.actual_mode(mode),.applied_seq(32'd0),.applied_frame(32'd0),.mode_toggle(t1),.desired_mode(m1),.desired_seq(s1));
 integer a,b,kind,n;
 task frame;
 input integer requested;
 input integer number;
 begin
  parking=requested==1;panorama=requested==2;raster=number;
  repeat(10)@(negedge vc);a=sent0;b=sent1;
  vs=1;repeat(4)@(negedge vc);vs=0;repeat(10)@(negedge vc);
  de=1;repeat(4096)@(negedge vc);de=0;repeat(25000)@(negedge nc);
  if(sent0-a!=(requested==0?2:1)||sent1-b!=(requested==0?2:0))
   $fatal(1,"mode=%0d raster=%0d primary=%0d secondary=%0d expected primary=%0d",requested,number,sent0-a,sent1-b,requested==0?2:1);
  if(f0||f1)$fatal(1,"packet overflow");
 end
 endtask
 initial begin
  #100;rstn=1;repeat(30)@(negedge nc);
  force net0.mac_ready=1;force net1.mac_ready=1;
  force net0.link=1;force net0.mac.peer_known=1;force net0.mac.peer_mac=48'h020000000001;
  force net1.link=1;force net1.mac.peer_known=1;force net1.mac.peer_mac=48'h020000000001;
  frame(1,1);frame(1,2);frame(1,3);frame(1,4);frame(2,5);frame(0,6);
  $display("PASS PARK odd/even all frames, primary-only composite, PANORAMA unchanged, four DRIVE streams preserved");$finish;
 end
 initial begin #10000000;$fatal(1,"timeout");end
endmodule
