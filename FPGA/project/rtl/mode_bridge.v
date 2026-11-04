`timescale 1ns/1ps
// Bundled values remain stable until the returned toggle is observed.
// Apply only at a raster frame boundary; no mid-line geometry changes.
module mode_bridge(input vclk,rstn,boundary,input [31:0] frame_count,
 input request_toggle,input [1:0] desired_mode,input [31:0] desired_seq,
 output reg applied_toggle,output reg [1:0] parking,output reg [31:0] applied_seq,applied_frame);
 (* ASYNC_REG="TRUE" *)reg [1:0] sync_req;
 always @(posedge vclk or negedge rstn)begin
   if(!rstn)begin sync_req<=0;applied_toggle<=0;parking<=0;applied_seq<=0;applied_frame<=0;end
   else begin
     sync_req<={sync_req[0],request_toggle};
     if(boundary&&sync_req[1]!=applied_toggle)begin
       parking<=desired_mode;applied_seq<=desired_seq;applied_frame<=frame_count+1'b1;applied_toggle<=sync_req[1];
     end
   end
 end
endmodule
