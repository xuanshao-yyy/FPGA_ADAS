`timescale 1ns / 1ps
// Single-master, open-drain I2C. MS72xx register address is LOW byte first.
// 16-bit 寄存器地址的单事务 I2C 主机；SCL/SDA 只主动拉低，释放后由外部上拉。
// MS72xx 默认低地址字节先发；OV5640 通过 HIGH_BYTE_FIRST=1 使用高字节先发。
module i2c_reg16 #(
    parameter integer HIGH_BYTE_FIRST = 0,
    parameter integer HALF_CYCLES = 250,
    parameter integer TIMEOUT_CYCLES = 1000000
) (
    input wire clk, rst_n, start, read_en,
    input wire [6:0] dev_addr,
    input wire [15:0] reg_addr,
    input wire [7:0] wr_data,
    output reg [7:0] rd_data,
    output reg busy, done, error,
    inout wire scl, sda
);

    // 状态顺序：START → 发送设备地址/寄存器地址 → ACK → 写数据或重复 START 读数据 → NACK/STOP。
    localparam IDLE = 0,
        START = 1, SEND_L = 2, SEND_H = 3, SEND_END = 4, ACK_L = 5, ACK_H = 6, ACK_END = 7,
        RESTART_L = 8, RESTART_H = 9, RESTART_S = 10, READ_L = 11, READ_H = 12, READ_END = 13,
        NACK_L = 14, NACK_H = 15, NACK_END = 16, STOP_L = 17, STOP_H = 18, STOP_END = 19;
    reg [4:0] state;
    reg scl_low, sda_low;
    reg [31:0] divider, watchdog;
    reg [7:0] shift, dev, lo, hi, data;
    reg reading;
    reg [2:0] bit_no, byte_no;
    reg [1:0] scl_sync, sda_sync;
    assign scl = scl_low ? 1'b0 : 1'bz;
    assign sda = sda_low ? 1'b0 : 1'bz;

    // 先同步总线实际电平；释放 SCL 后等待其真正变高，而不是只检查内部驱动信号。
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            scl_sync <= 2'b11;
            sda_sync <= 2'b11;
        end else begin
            scl_sync <= {scl_sync[0], scl};
            sda_sync <= {sda_sync[0], sda};
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            scl_low <= 0;
            sda_low <= 0;
            divider <= 0;
            watchdog <= 0;
            busy <= 0;
            done <= 0;
            error <= 0;
            rd_data <= 0;
            shift <= 0;
            dev <= 0;
            lo <= 0;
            hi <= 0;
            data <= 0;
            reading <= 0;
            bit_no <= 7;
            byte_no <= 0;
        end else begin

            // done 是单拍完成脉冲；error 与该次完成一同有效。watchdog 限制整笔事务最长时间。
            done <= 0;
            if (state == IDLE) begin
                divider  <= 0;
                watchdog <= 0;
                if (start) begin
                    dev <= {dev_addr, 1'b0};
                    lo <= reg_addr[7:0];
                    hi <= reg_addr[15:8];
                    data <= wr_data;
                    reading <= read_en;
                    shift <= {dev_addr, 1'b0};
                    bit_no <= 7;
                    byte_no <= 0;
                    busy <= 1;
                    error <= 0;
                    rd_data <= 0;
                    state <= START;
                end
            end else if (watchdog == TIMEOUT_CYCLES - 1) begin
                scl_low <= 0;
                sda_low <= 0;
                busy <= 0;
                done <= 1;
                error <= 1;
                state <= IDLE;
            end else begin
                watchdog <= watchdog + 1'b1;
                if (divider == HALF_CYCLES - 1) begin
                    divider <= 0;
                    case (state)
                        START: if (scl_sync[1] && sda_sync[1]) begin
                            sda_low <= 1;
                            state   <= SEND_L;
                        end
                        SEND_L: begin
                            scl_low <= 1;
                            sda_low <= ~shift[bit_no];
                            state   <= SEND_H;
                        end
                        SEND_H: begin
                            scl_low <= 0;
                            state   <= SEND_END;
                        end
                        SEND_END: if (scl_sync[1]) begin
                            scl_low <= 1;
                            if (bit_no == 0) state <= ACK_L;
                            else begin
                                bit_no <= bit_no - 1'b1;
                                state  <= SEND_L;
                            end
                        end
                        ACK_L: begin
                            sda_low <= 0;
                            state   <= ACK_H;
                        end
                        ACK_H: begin
                            scl_low <= 0;
                            state   <= ACK_END;
                        end

                        // 每个发送字节都检查应答；NACK 时置 error 并进入 STOP，不继续写后续字节。
                        ACK_END: if (scl_sync[1]) begin
                            scl_low <= 1;
                            if (sda_sync[1]) begin
                                error <= 1;
                                state <= STOP_L;
                            end else begin
                                bit_no <= 7;
                                case (byte_no)
                                    0: begin
                                        shift   <= HIGH_BYTE_FIRST ? hi : lo;
                                        byte_no <= 1;
                                        state   <= SEND_L;
                                    end
                                    1: begin
                                        shift   <= HIGH_BYTE_FIRST ? lo : hi;
                                        byte_no <= 2;
                                        state   <= SEND_L;
                                    end
                                    2: if (reading) state <= RESTART_L;
                                    else begin
                                        shift   <= data;
                                        byte_no <= 3;
                                        state   <= SEND_L;
                                    end
                                    3: state <= STOP_L;
                                    4: state <= READ_L;
                                    default: state <= STOP_L;
                                endcase
                            end
                        end

                        // 读事务保留总线并发起重复 START，发送读方向设备地址后接收一个字节。
                        RESTART_L: begin
                            sda_low <= 0;
                            state   <= RESTART_H;
                        end
                        RESTART_H: begin
                            scl_low <= 0;
                            state   <= RESTART_S;
                        end
                        RESTART_S: if (scl_sync[1]) begin
                            sda_low <= 1;
                            shift   <= dev | 8'h01;
                            byte_no <= 4;
                            state   <= SEND_L;
                        end
                        READ_L: begin
                            scl_low <= 1;
                            sda_low <= 0;
                            state   <= READ_H;
                        end
                        READ_H: begin
                            scl_low <= 0;
                            state   <= READ_END;
                        end
                        READ_END: if (scl_sync[1]) begin
                            rd_data[bit_no] <= sda_sync[1];
                            scl_low <= 1;
                            if (bit_no == 0) state <= NACK_L;
                            else begin
                                bit_no <= bit_no - 1'b1;
                                state  <= READ_L;
                            end
                        end

                        // 单字节读取后由主机发 NACK，再产生 STOP 结束本次事务。
                        NACK_L: begin
                            sda_low <= 0;
                            state   <= NACK_H;
                        end
                        NACK_H: begin
                            scl_low <= 0;
                            state   <= NACK_END;
                        end
                        NACK_END: if (scl_sync[1]) begin
                            scl_low <= 1;
                            state   <= STOP_L;
                        end
                        STOP_L: begin
                            scl_low <= 1;
                            sda_low <= 1;
                            state   <= STOP_H;
                        end
                        STOP_H: begin
                            scl_low <= 0;
                            state   <= STOP_END;
                        end
                        STOP_END: if (scl_sync[1]) begin
                            sda_low <= 0;
                            busy <= 0;
                            done <= 1;
                            state <= IDLE;
                        end
                        default: state <= IDLE;
                    endcase
                end else divider <= divider + 1'b1;
            end
        end
    end
endmodule
