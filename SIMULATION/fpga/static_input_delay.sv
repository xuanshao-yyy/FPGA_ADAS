`timescale 1ns/1ps
// Simulation acceleration only. The design uses fixed LOAD_N=0/MOVE=0 and
// pixel transitions much slower than one 25ps tap. Collapse the vendor's 127
// event stages to the same total delay; tb_input_delay compares both models.
`ifdef FAST_IO_DELAY
module GTP_IODELAY
`else
module GTP_IODELAY_fast
`endif
 #(parameter [6:0] DELAY_STEP=0,parameter DELAY_DEPTH=7)
 (output DO,DELAY_OB,input DI,LOAD_N,MOVE,DIRECTION);
 assign #(DELAY_STEP*0.025) DO=DI;
 assign DELAY_OB=(DIRECTION&&DELAY_STEP==0)||(!DIRECTION&&DELAY_STEP==127);
 initial begin #1;if(LOAD_N!==0||MOVE!==0||DIRECTION!==0||DELAY_DEPTH!=7)$fatal(1,"Fast delay model requires static configuration");end
endmodule
