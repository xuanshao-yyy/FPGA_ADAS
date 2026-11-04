`timescale 1ns / 1ps
// 通过 DDR 输出单元转发 HDMI 像素时钟，保持既有 GTP_OSERDES 接线和初始化设置。
module hdmi_forward_clock (
    input wire pclk,
    output wire forwarded
);
    // Logos exposes DDR output through GTP_OSERDES, not legacy GTP_ODDR.
    // DI[0]=1, DI[1]=0 reproduces the source clock after I/O pipeline startup.

    // serial_clock 是 DDR 单元输出，tri_state 是其三态控制，经输出缓冲送到板外。
    wire serial_clock, tri_state;
    GTP_OSERDES #(
        .OSERDES_MODE("ODDR"),
        .WL_EXTEND("FALSE"),
        .GRS_EN("TRUE"),
        .LRS_EN("TRUE"),
        .TSDDR_INIT(1'b0)
    ) clock_oddr (
        .DO(serial_clock),
        .TQ(tri_state),
        .DI(8'h01),
        .TI(4'h0),
        .RCLK(pclk),
        .SERCLK(pclk),
        .OCLK(1'b0),
        .RST(1'b0)
    );
    GTP_OUTBUFT clock_buffer (
        .I(serial_clock),
        .T(tri_state),
        .O(forwarded)
    );
endmodule
