`timescale 1ns / 1ps
// CAM1 的 SCCB 配置与读回校验；先核对芯片 ID，再写表，最后验证关键寄存器。
// slow_camera 在启动时锁存，选择既有的 15FPS 配置；不会在运行中直接切换时钟。
module ov5640_init #(
    parameter HALF_CYCLES = 1667,  // ~10kHz SCCB at 50MHz (three phases per bit)
    parameter RESET_WAIT = 5000000, GAP = 50000, SETTLE = 15000000
) (
    input clk, rstn, enable,
    inout scl, sda,
    output reg ready, fault,
    output reg id_ok,
    output reg [8:0] index,
    input test_pattern,
    input slow_camera
);

    // state：0 等待使能，1 启动事务，2 等完成，3 间隔等待，5 初始化完成。
    // phase：0/1 读取芯片 ID 两字节，2 写表，3 读回关键配置。
    reg [2:0] state, phase;
    reg [3:0] verify_index;
    reg pattern_mode, slow_mode;
    reg [31:0] delay_count;
    reg start, reading;
    reg [15:0] addr;
    reg [7:0] data;
    wire [23:0] command;
    reg [23:0] expected;
    wire [7:0] result;
    wire done, error, busy;
    ov5640_rom rom (
        index,
        command
    );

    // Read back actual hardware settings after the sensor has settled.

    // 读回项必须与最终写入值一致，包括启动时锁存的慢速模式与测试图选择。
    always @*
        case (verify_index)
            0: expected = 24'h430060;
            1: expected = 24'h501f01;
            2: expected = {16'h3035, slow_mode ? 8'h41 : 8'h21};
            3: expected = 24'h474020;
            4: expected = {16'h503d, pattern_mode ? 8'h80 : 8'h00};
            5: expected = 24'h380805;
            6: expected = 24'h380900;
            7: expected = 24'h380a02;
            8: expected = 24'h380bd0;
            default: expected = 0;
        endcase
    i2c_reg16 #(
        .HIGH_BYTE_FIRST(1),
        .HALF_CYCLES(HALF_CYCLES),
        .TIMEOUT_CYCLES(5000000)
    ) bus (
        .clk(clk),
        .rst_n(rstn),
        .start(start),
        .read_en(reading),
        .dev_addr(7'h3c),
        .reg_addr(addr),
        .wr_data(data),
        .rd_data(result),
        .busy(busy),
        .done(done),
        .error(error),
        .scl(scl),
        .sda(sda)
    );

    // Verify both chip-ID bytes before programming. Never advance after NACK.
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            state <= 0;
            phase <= 0;
            delay_count <= 0;
            start <= 0;
            reading <= 0;
            addr <= 0;
            data <= 0;
            ready <= 0;
            fault <= 0;
            id_ok <= 0;
            index <= 0;
            verify_index <= 0;
            pattern_mode <= 0;
            slow_mode <= 0;
        end else begin
            start <= 0;
            case (state)
                0: if (enable) begin
                    pattern_mode <= test_pattern;
                    slow_mode <= slow_camera;
                    delay_count <= RESET_WAIT;
                    state <= 3;
                end

                // 索引 208 覆盖时钟分频值；258 覆盖测试图值；其余表项沿用 ROM。
                1: begin
                    reading <= phase != 2;
                    addr <= phase == 0 ? 16'h300a :
                        phase == 1 ? 16'h300b : phase == 3 ? expected[23:8] : command[23:8];
                    data <= index == 258 ? (pattern_mode ? 8'h80 : 8'h00) :
                        index == 208 ? (slow_mode ? 8'h41 : 8'h21) : command[7:0];
                    start <= 1;
                    state <= 2;
                end
                2: if (done) begin
                    state <= 3;
                    delay_count <= GAP;
                    if (error || (phase == 0 && result != 8'h56) || (phase == 1 && result != 8'h40))
                        fault <= 1;

                    // 关键配置读回不符时，从配置表头重写并再次验证；错误事务本身不推进索引。
                    else if (phase == 3 && result != expected[7:0]) begin
                        fault <= 1;
                        phase <= 2;
                        index <= 0;
                        verify_index <= 0;
                        delay_count <= RESET_WAIT;
                    end else if (phase < 2) begin
                        phase <= phase + 1'b1;
                        if (phase == 1) id_ok <= 1;
                    end else if (phase == 2) begin
                        if (index == 258) begin
                            phase <= 3;
                            verify_index <= 0;
                            delay_count <= SETTLE;
                        end else begin
                            index <= index + 1'b1;
                            if (index == 1) delay_count <= RESET_WAIT;
                        end
                    end else if (verify_index == 8) begin
                        ready <= 1;
                        fault <= 0;
                        state <= 5;
                    end else verify_index <= verify_index + 1'b1;
                end
                3: if (delay_count == 0) state <= 1;
                else delay_count <= delay_count - 1'b1;
                5: state <= 5;
                default: state <= 0;
            endcase
        end
    end
endmodule
