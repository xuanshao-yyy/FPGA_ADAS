`timescale 1ns / 1ps
// Register values in ms72xx_rom.v are derived from the supplied Meyesemi demos.
// New sequencer: verify scratch register, check ACKs, retry, and poll RX readiness.
// HDMI 芯片初始化调度：探测寄存器 → 写配置表 → 轮询接收状态 → 按既有条件校准。
// ready 表示曾完成相应初始化阶段，不是实时的视频输入锁定指示。
module ms72xx_sequencer #(
    parameter integer IS_RX = 1,
    parameter integer GAP_CYCLES = 50000,
    parameter integer SETTLE_CYCLES = 16000000
) (
    input wire clk, rst_n, enable,
    input wire grant, done, error,
    input wire [7:0] received,
    output wire request,
    output reg start, reading,
    output reg [15:0] addr,
    output reg [7:0] data,
    output reg ready,
    output reg fault,
    output wire [9:0] debug_index,
    output reg debug_probe_ok, debug_table_done, debug_poll_valid,
    output reg [7:0] debug_poll_data
);

    // state 负责事务调度；phase 负责当前操作类型。两者分开，重试时不会跳过当前表项。
    localparam BOOT = 0, ISSUE = 1, WAIT_DONE = 2, GAP = 3, RX_POLL = 4, TX_DONE = 5;
    reg [2:0] state;
    reg [2:0] phase;  // 0 scratch write, 1 scratch read, 2 table, 3 poll, 4 calibration
    reg [9:0] index;
    reg [1:0] poll_index, previous_flags;
    reg recalibrate;
    reg [31:0] delay_count;
    wire [23:0] command;
    wire [9:0] last_index, calibration_index;
    assign debug_index = index;
    ms72xx_rom #(
        .IS_RX(IS_RX)
    ) rom (
        index,
        command,
        last_index,
        calibration_index
    );
    assign request = (state == ISSUE);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= BOOT;
            phase <= 0;
            index <= 0;
            delay_count <= 0;
            ready <= 0;
            fault <= 0;
            start <= 0;
            reading <= 0;
            addr <= 0;
            data <= 0;
            debug_probe_ok <= 0;
            debug_table_done <= 0;
            debug_poll_valid <= 0;
            debug_poll_data <= 0;
            poll_index <= 0;
            previous_flags <= 0;
            recalibrate <= 0;
        end else begin
            start <= 0;
            case (state)
                BOOT: if (enable) begin
                    phase <= 0;
                    index <= 0;
                    state <= ISSUE;
                end
                ISSUE: if (grant) begin
                    if (phase == 0 || phase == 1) begin
                        addr <= 16'h0003;
                        data <= 8'h5a;
                        reading <= (phase == 1);
                    end else if (phase == 3) begin
                        addr <= 16'h209c + {14'd0, poll_index};
                        data <= 0;
                        reading <= 1;
                    end else begin
                        addr <= command[23:8];
                        data <= command[7:0];
                        reading <= 0;
                    end
                    start <= 1;
                    state <= WAIT_DONE;
                end
                WAIT_DONE: if (done) begin
                    delay_count <= GAP_CYCLES;
                    state <= GAP;
                    if (error || (phase == 1 && received != 8'h5a)) begin
                        fault <= 1;
                        // Retry the same operation; never advance on NACK or timeout.
                    end else begin
                        fault <= 0;
                        case (phase)
                            0: phase <= 1;
                            1: begin
                                phase <= 2;
                                index <= 0;
                                debug_probe_ok <= 1;
                            end
                            2: begin
                                if (IS_RX && index == calibration_index - 1) begin
                                    phase <= 3;
                                    state <= RX_POLL;
                                    debug_table_done <= 1;
                                end else if (!IS_RX && index == last_index) begin
                                    ready <= 1;
                                    state <= TX_DONE;
                                end else begin
                                    index <= index + 1'b1;
                                    if (!IS_RX && index == 18) delay_count <= SETTLE_CYCLES;
                                end
                            end

                            // 轮询依次读取 0x209C..0x209F；只在 0x209E 低两位的 00→10 跳变时安排校准。
                            // 这是既有行为：未加入无帧超时恢复，也未把 ready 改成连续锁定门控。
                            3: begin
                                // Restore the reference's four single-register reads in order.
                                // 00->10 triggers calibration; it is NOT a continuous ready gate.
                                if (poll_index == 2) begin
                                    debug_poll_data <= received;
                                    if (previous_flags == 0 && received[1:0] == 2) recalibrate <= 1;
                                    previous_flags <= received[1:0];
                                end
                                if (poll_index == 3) begin
                                    debug_poll_valid <= 1;
                                    poll_index <= 0;
                                    if (recalibrate) begin
                                        phase <= 4;
                                        index <= calibration_index;
                                        recalibrate <= 0;
                                    end
                                end else poll_index <= poll_index + 1'b1;
                            end

                            // 七笔校准写完后置 ready，并回到持续轮询；ready 保留“曾初始化完成”的含义。
                            4: begin
                                if (index == last_index) begin
                                    ready <= 1;
                                    phase <= 3;
                                    poll_index <= 0;
                                end else index <= index + 1'b1;
                            end
                            default: phase <= 0;
                        endcase
                    end
                end
                GAP: if (delay_count == 0) state <= ISSUE;
                else delay_count <= delay_count - 1'b1;
                RX_POLL: begin
                    phase <= 3;
                    delay_count <= GAP_CYCLES;
                    state <= GAP;
                end
                TX_DONE: if (!enable) begin
                    ready <= 0;
                    state <= BOOT;
                end
                default: state <= BOOT;
            endcase
        end
    end
endmodule
