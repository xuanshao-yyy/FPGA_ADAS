`timescale 1ns/1ps
// Exercise the real network_port instances, RAM metadata, mux and MAC together.
// PCS link and learned peer are fixture inputs; PHY timing/ARP covered separately.
module tb_network_panorama;
 reg nc=0,vc=0,rstn=0,vs=0,de=0,panorama=1;
 always #4 nc=~nc;always #13.469 vc=~vc;
 wire [7:0] txd;wire txk,disparity,link,known,fault,mt;wire [1:0] dm;wire [31:0] sent,ds;
 network_port #(.PRODUCT(1),.PRIMARY(1),.IMAGE_BYTES(8192),.SLOT_BITS(1)) dut(
 .nclk(nc),.arstn(rstn),.synced(1'b1),.rxd(8'hbc),.rxk(1'b1),.rxerr(1'b0),.txd(txd),.txk(txk),.disparity(disparity),
 .vclk(vc),.vrstn(rstn),.vs(vs),.de(de),.pixels(32'h24681357),.source_seq(64'h100000001),.link(link),.known(known),.fault(fault),.sent(sent),
 .diagnostic_data(768'd0),.parking(1'b0),.panorama(panorama),.quad_pixel(16'd0),.raster_frame(32'd1),
 .applied_toggle(1'b0),.actual_mode(2'd2),.applied_seq(32'd0),.applied_frame(32'd0),.mode_toggle(mt),.desired_mode(dm),.desired_seq(ds));
 integer fd,n,count=0;reg [31:0] tick=0;
 always @(posedge nc)begin
  tick<=tick+1;
  if(dut.mac_valid&&dut.mac_ready)begin $fwrite(fd,"%02x",dut.mac_txd);count<=count+1;end
 end
 initial begin
  fd=$fopen("network_panorama.hex","w");#100;rstn=1;
  repeat(30)@(negedge nc);
  force dut.link=1;force dut.mac.peer_known=1;force dut.mac.peer_mac=48'h020000000001;
  force dut.mac_ready=0;
  @(negedge vc);vs=1;repeat(4)@(negedge vc);vs=0;
  repeat(10)@(negedge vc);de=1;
  repeat(4096)@(negedge vc);de=0;
  repeat(10)@(negedge nc);
  if(dut.composites[1:0]!==2||dut.port!==5200)$fatal(1,"Network mode metadata truncated: %b port=%d",dut.composites,dut.port);
  // Mode changes while an old-mode packet is queued must not relabel its header.
  panorama=0;
  force dut.mac_ready=(tick%17)<12;
  wait(sent==1);repeat(10)@(negedge nc);$fclose(fd);
  if(fault||count!=8274)$fatal(1,"Invalid network output fault=%b bytes=%d",fault,count);
  $display("PASS: real network_port two-bit queued metadata, QV04 port retained through mode change, 8192 RGB565 payload and TX stalls");$finish;
 end
 initial begin #1000000;$fatal(1,"Timeout");end
endmodule
