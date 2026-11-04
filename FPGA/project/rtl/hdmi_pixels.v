`timescale 1ns / 1ps
// Use the verified IN1/IN3 delayed input path on all three ports. IN2 also
// requires delay in the combined placement to meet fast-corner input setup.
// HDMI 并行输入采样：VS、DE、RGB 使用相同延迟，再经下降沿/上升沿两级寄存。
// DELAY_TAPS 是现有时序配置，本次注释整理不调整采样相位或延迟值。
module hdmi_pixels #(
    parameter DELAY_TAPS = 127
) (
    input pclk, rstn,
    input vs, de,
    input [23:0] rgb,
    output [25:0] data
);
    (* syn_useioff=0 *) reg [25:0] first, captured;
    wire [25:0] raw = {vs, de, rgb};
    wire [25:0] delayed;

    // 26 根并行输入逐根加入同样的 I/O 延迟，保持像素与同步信号一起采样。
    genvar k;
    generate
        for (k = 0; k < 26; k = k + 1) begin : input_delay
            if (DELAY_TAPS != 0) begin
                GTP_IODELAY #(
                    .DELAY_STEP(DELAY_TAPS)
                ) delay_cell (
                    .DI(raw[k]),
                    .DO(delayed[k]),
                    .DELAY_OB(),
                    .LOAD_N(1'b0),
                    .MOVE(1'b0),
                    .DIRECTION(1'b0)
                );
            end else assign delayed[k] = raw[k];
        end
    endgenerate

    // 先在下降沿采样延迟后的总线，再在上升沿交给后续逻辑；保持原有两级时序。
    always @(negedge pclk) first <= delayed;
    always @(posedge pclk or negedge rstn)
        if (!rstn) captured <= 0;
        else captured <= first;
    assign data = captured;
endmodule
