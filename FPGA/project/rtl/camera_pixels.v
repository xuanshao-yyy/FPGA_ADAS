`timescale 1ns / 1ps

// DVP byte receiver. Sample data, HREF and VSYNC together on PCLK.
// Assemble each two-byte sensor BGR565 word into the internal RGB565 order.
// Clock rate and sensor configuration are controlled by ov5640_init.
// CAM1 的 DVP 输入采样与像素拼接；每两个有效字节形成一个内部 RGB565 像素。
// de 是像素有效脉冲，line_active 是行有效电平，两者不能混用。
module camera_pixels (
    input pclk, rstn,
    input vs, href,
    input [7:0] din,
    output reg de, line_active, vs_d,
    output reg [15:0] pixel
);

    // Reset-free first stage can be packed into input I/O cells.
    reg [7:0] d0, byte_hi;
    reg h0, v0, phase;

    // 第一级同时采样数据、HREF 和 VSYNC，不加入单独的数据延迟。
    always @(posedge pclk) begin
        d0 <= din;
        h0 <= href;
        v0 <= vs;
    end

    // 第二级在行内交替缓存第一个字节并拼成像素；帧同步或行间隙清除字节相位。
    always @(posedge pclk or negedge rstn) begin
        if (!rstn) begin
            byte_hi <= 0;
            phase <= 0;
            de <= 0;
            line_active <= 0;
            vs_d <= 0;
            pixel <= 0;
        end else begin
            vs_d <= v0;
            line_active <= h0;
            de <= 0;
            if (v0 || !h0) phase <= 0;
            else begin
                phase <= ~phase;
                if (!phase) byte_hi <= d0;
                else begin

                    // 4300=60 in the supplied table uses the demo's BGR565 bus ordering.
                    pixel <= {d0[4:0], byte_hi[2:0], d0[7:5], byte_hi[7:3]};
                    de <= 1;
                end
            end
        end
    end
endmodule
