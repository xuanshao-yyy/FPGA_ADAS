`timescale 1ns/1ps
// Source-owned bundled snapshot, request/ack toggles. No free-running binary
// counter is sampled bit-by-bit across clock domains. A stopped source clock
// leaves its last completed snapshot intact rather than blocking other probes.
module diag_snapshot #(parameter WIDTH=256)(
 input sc, srstn, input [WIDTH-1:0] source,
 input dc, drstn, output reg [WIDTH-1:0] value
);
 reg request;
 (* ASYNC_REG="TRUE" *) reg [1:0] req_sync, ack_sync;
 reg ack;
 reg [WIDTH-1:0] held;
 reg [2:0] settle;
 always @(posedge sc or negedge srstn)
  if(!srstn)begin req_sync<=0;ack<=0;held<=0;end
  else begin
   req_sync<={req_sync[0],request};
   if(req_sync[1]!=ack)begin held<=source;ack<=req_sync[1];end
  end
 always @(posedge dc or negedge drstn)
  if(!drstn)begin request<=0;ack_sync<=0;settle<=0;value<=0;end
  else begin
   ack_sync<={ack_sync[0],ack};
   if(ack_sync[1]==request)begin
    if(settle==7)begin value<=held;request<=~request;settle<=0;end
    else settle<=settle+1'b1;
   end else settle<=0;
  end
endmodule
