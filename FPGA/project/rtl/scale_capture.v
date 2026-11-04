`timescale 1ns / 1ps

// Measure the previous progressive frame; nearest-neighbour downscale using
// integer accumulators. The source keeps its original resolution and clock.
// Final packet is withheld until VS validates the entire source frame.
// 按上一帧测得的宽高做最近邻降采样，输入源的分辨率和 PCLK 不变。
// 每 16 个 RGB565 像素打包为 256 bit，并用两个标志位跨时钟送往 DDR。
module scale_capture #(
    parameter WIDTH = 640, HEIGHT = 360, DIAGNOSTICS=0
) (
    input pclk, rstn, vs, line_active, valid,
    input [15:0] pixel,
    input dclk, drstn, pop,
    output [257:0] data,
    output empty,
    output reg [11:0] source_width, source_height,
    output reg [31:0] dropped,
    output [255:0] diagnostic
);
    localparam PIXELS = WIDTH * HEIGHT;
    reg vs_d, line_d, active, first, bad, tail;
    reg [11:0] x, lines, measured_width, ax, ay;
    reg [$clog2(PIXELS)-1:0] pixels;
    reg [3:0] lane;
    reg [255:0] pack;
    wire start = vs && !vs_d;
    wire line_end = !line_active && line_d;

    // ax/ay 为水平/垂直取样累加器；跨过原图宽/高时选取该像素/行，并减去相应尺寸。
    wire [12:0] sx = {1'b0, ax} + WIDTH;
    wire [12:0] sy = {1'b0, ay} + HEIGHT;
    wire take = active && valid && line_active && !start && !tail && (sx >= source_width) &&
        (sy >= source_height);
    wire full;
    wire last_pixel = (pixels == PIXELS - 1);

    // 只有行宽一致、尺寸在允许范围内且帧完整结束，才允许发布最后一个数据字。
    wire geometry_ok = !bad && (measured_width >= WIDTH) && (measured_width <= 1920) &&
        (lines >= HEIGHT) && (lines <= 1080) && !line_active;
    wire commit = start && active && tail && geometry_ok && (measured_width == source_width) &&
        (lines == source_height) && !full;
    wire emit = take && (lane == 15) && !last_pixel && !full;

    generate if(DIAGNOSTICS)begin: debug_capture
      reg [31:0] frames,commits,geometry_failed,size_changed,fifo_failed,no_tail;
      always @(posedge pclk or negedge rstn)
       if(!rstn)begin frames<=0;commits<=0;geometry_failed<=0;size_changed<=0;fifo_failed<=0;no_tail<=0;end
       else begin
        if(start)begin
         frames<=frames+1'b1;
         if(commit)commits<=commits+1'b1;
         if(!geometry_ok)geometry_failed<=geometry_failed+1'b1;
         if(geometry_ok && active && (measured_width!=source_width || lines!=source_height))size_changed<=size_changed+1'b1;
         if(active && !tail)no_tail<=no_tail+1'b1;
        end
        if((take && lane==15 && !last_pixel && full) || (start && active && tail && full))fifo_failed<=fifo_failed+1'b1;
       end
      assign diagnostic={frames,commits,geometry_failed,size_changed,fifo_failed,no_tail,{8'd0,source_width,source_height},dropped};
    end else begin: debug_disabled
      assign diagnostic=0;
    end endgenerate
    // FIFO 数据布局：bit257=帧尾，bit256=帧首，bit255:0=16 个 RGB565 像素。
    // 帧尾字暂存到下一个 VS 上升沿；这样 DDR 只能发布已经校验完整的帧。
    dual_fifo fifo (
        .wc(pclk),
        .wrstn(rstn),
        .push(emit || commit),
        .din(commit ? {1'b1, first, pack} : {1'b0, first, pixel, pack[255:16]}),
        .full(full),
        .rc(dclk),
        .rrstn(drstn),
        .pop(pop),
        .dout(data),
        .empty(empty)
    );

    always @(posedge pclk or negedge rstn) begin
        if (!rstn) begin
            vs_d <= 0;
            line_d <= 0;
            active <= 0;
            first <= 1;
            bad <= 0;
            tail <= 0;
            x <= 0;
            lines <= 0;
            measured_width <= 0;
            source_width <= 0;
            source_height <= 0;
            ax <= 0;
            ay <= 0;
            pixels <= 0;
            lane <= 0;
            pack <= 0;
            dropped <= 0;
        end else begin
            vs_d   <= vs;
            line_d <= line_active;

            // 帧边界校验刚结束的画面，并把测得尺寸用于下一帧；启动/格式变化时先重新测量。
            if (start) begin
                if (active && !commit) dropped <= dropped + 1;
                source_width <= geometry_ok ? measured_width : 0;
                source_height <= geometry_ok ? lines : 0;
                active <= geometry_ok;
                first <= 1;
                bad <= 0;
                tail <= 0;
                x <= 0;
                lines <= 0;
                measured_width <= 0;
                ax <= 0;
                ay <= 0;
                pixels <= 0;
                lane <= 0;
            end else begin
                if (valid && line_active) begin
                    if (x < 4095) x <= x + 1'b1;
                    else bad <= 1;
                    if (active) ax <= sx >= source_width ? sx - source_width : sx;
                end
                if (line_end) begin
                    if (lines == 0) measured_width <= x;
                    else if (x != measured_width) bad <= 1;
                    if (lines < 4095) lines <= lines + 1'b1;
                    else bad <= 1;
                    x  <= 0;
                    ax <= 0;
                    if (active) ay <= sy >= source_height ? sy - source_height : sy;
                end

                // 按 16 像素打包；FIFO 满时放弃当前帧，下一帧首标志会让 DDR 丢弃未完成的旧帧。
                if (take) begin
                    pack   <= {pixel, pack[255:16]};
                    lane   <= lane + 1'b1;
                    pixels <= pixels + 1'b1;
                    if (last_pixel) tail <= 1;
                    else if (lane == 15) begin
                        first <= 0;
                        if (full) begin
                            active  <= 0;
                            dropped <= dropped + 1;
                        end
                    end
                end
            end
        end
    end
endmodule
