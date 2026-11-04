`timescale 1ns/1ps
// One owner per lease. A successful SET is reported APPLIED only after the
// video-domain acknowledgement. Repeated SETs are idempotent.
module control_mode #(parameter LEASE_CYCLES=625000000)(
 input clk,rstn,link,request,input [31:0] session,seq,input [7:0] operation,mode,
 input applied_toggle,input [1:0] actual_mode,input [31:0] applied_seq,applied_frame,
 output reg mode_toggle,output reg [1:0] desired_mode,output reg [31:0] desired_seq,
 input reply_take,output reg reply_pending,output reg [223:0] reply);
 reg [31:0] owner,last_seq,lease;
 reg [1:0] ack_sync;
 reg ack_seen,pending;
 reg [31:0] actual_seq,actual_frame;
 reg [1:0] actual;
 reg [7:0] code;
 wire busy=pending;
 always @*begin
     code=0;
     if(operation==1)begin
         if(owner!=session)code=2;
         else if(seq==0)code=4;
         else if(seq==last_seq)code=pending?1:0;
         else if(pending)code=3;
         else if(seq==0||seq<=last_seq)code=4;
         else code=1;
     end else if(owner!=0&&owner!=session)code=2;
     else if(pending)code=1;
 end
 // CRC is serialized by the MAC, not a long combinational chain here.
 always @(posedge clk or negedge rstn)begin
     if(!rstn)begin owner<=0;last_seq<=0;lease<=0;ack_sync<=0;ack_seen<=0;pending<=0;mode_toggle<=0;desired_mode<=0;desired_seq<=0;actual_seq<=0;actual_frame<=0;actual<=0;reply_pending<=0;reply<=0;end
     else begin
         ack_sync<={ack_sync[0],applied_toggle};
         if(reply_take)reply_pending<=0;
         if(ack_sync[1]!=ack_seen)begin ack_seen<=ack_sync[1];pending<=0;actual<=actual_mode;actual_seq<=applied_seq;actual_frame<=applied_frame;end
         if(owner!=0&&lease<LEASE_CYCLES)lease<=lease+1'b1;
         if((!link||lease>=LEASE_CYCLES)&&!pending)begin
             if(actual)begin desired_mode<=0;desired_seq<=last_seq;mode_toggle<=~mode_toggle;pending<=1;end
             else begin owner<=0;last_seq<=0;lease<=0;end
         end
         if(request)begin
             if(operation==0&&owner==0&&!actual&&!pending)begin owner<=session;lease<=0;last_seq<=0;end
             if(owner==session)lease<=0;
             if(operation==1&&owner==session&&code==1&&!pending&&seq>last_seq)begin
                 desired_mode<=mode[1:0];desired_seq<=seq;mode_toggle<=~mode_toggle;pending<=1;last_seq<=seq;
             end
             // Header + 24-byte status body; final CRC32 filled by the MAC.
             reply<={32'h51413031,session,seq,6'd0,actual,code,15'd0,(busy||code==1),actual_frame,actual_seq,32'd0};
             reply_pending<=1;
         end
     end
 end
endmodule
