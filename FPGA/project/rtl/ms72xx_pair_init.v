`timescale 1ns / 1ps
// IN3 MS7200 (0x2B) and OUT MS7210 (0x59) share a single physical I2C bus.
// A grant reserves the master through STOP/timeout; no byte-level switching.
// 同一物理 I2C 总线上的两个初始化序列，共用一个主机，并以完整事务为单位轮询授权。
// SECOND_RX=1 时第二个序列也是 HDMI 接收器；tx_* 名称仍表示第二个序列，不一定是发送芯片。
module ms72xx_pair_init #(
    parameter SECOND_RX = 0,
    parameter [6:0] FIRST_ADDR = 7'h2b, SECOND_ADDR = 7'h59,
    parameter integer GAP_CYCLES = 50000,
    parameter integer SETTLE_CYCLES = 16000000,
    parameter integer HALF_CYCLES = 250,
    parameter integer TIMEOUT_CYCLES = 1000000
) (
    input wire clk, rst_n,
    inout wire scl, sda,
    output wire rx_ready, tx_ready, rx_fault, tx_fault,
    output wire probe_ok, table_done, poll_valid,
    output wire [7:0] poll_data
);
    wire rx_request, tx_request, rx_start, tx_start, rx_read, tx_read;
    wire [15:0] rx_addr, tx_addr;
    wire [7:0] rx_data, tx_data, received;
    wire done, error, busy;

    // owner_tx 锁定当前事务归属；两个序列都请求时，与上一次使用者交替授权。
    reg active, owner_tx, last_owner_tx;
    wire select_tx = tx_request && (!rx_request || !last_owner_tx);
    wire rx_grant = !active && rx_request && !select_tx;
    wire tx_grant = !active && tx_request && select_tx;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            active <= 0;
            owner_tx <= 0;
            last_owner_tx <= 1;
        end else if (!active) begin
            if (rx_grant || tx_grant) begin
                active   <= 1;
                owner_tx <= tx_grant;
            end
        end else if (done) begin
            active <= 0;
            last_owner_tx <= owner_tx;
        end
    end

    // 第一个序列始终初始化接收器，使用 FIRST_ADDR。
    ms72xx_sequencer #(
        .IS_RX(1),
        .GAP_CYCLES(GAP_CYCLES),
        .SETTLE_CYCLES(SETTLE_CYCLES)
    ) receiver (
        .clk(clk),
        .rst_n(rst_n),
        .enable(1'b1),
        .grant(rx_grant),
        .request(rx_request),
        .start(rx_start),
        .reading(rx_read),
        .addr(rx_addr),
        .data(rx_data),
        .done(done && active && !owner_tx),
        .error(error),
        .received(received),
        .ready(rx_ready),
        .fault(rx_fault),
        .debug_index(),
        .debug_probe_ok(probe_ok),
        .debug_table_done(table_done),
        .debug_poll_valid(poll_valid),
        .debug_poll_data(poll_data)
    );

    // 第二个序列类型由 SECOND_RX 决定，使用 SECOND_ADDR；事务完成只通知当前 owner。
    ms72xx_sequencer #(
        .IS_RX(SECOND_RX),
        .GAP_CYCLES(GAP_CYCLES),
        .SETTLE_CYCLES(SETTLE_CYCLES)
    ) transmitter (
        .clk(clk),
        .rst_n(rst_n),
        .enable(1'b1),
        .grant(tx_grant),
        .request(tx_request),
        .start(tx_start),
        .reading(tx_read),
        .addr(tx_addr),
        .data(tx_data),
        .done(done && active && owner_tx),
        .error(error),
        .received(received),
        .ready(tx_ready),
        .fault(tx_fault),
        .debug_index(),
        .debug_probe_ok(),
        .debug_table_done(),
        .debug_poll_valid(),
        .debug_poll_data()
    );

    // 两个序列仅在授权时启动主机，owner 锁定到 STOP 或超时完成，避免事务中途切换地址。
    i2c_reg16 #(
        .HALF_CYCLES(HALF_CYCLES),
        .TIMEOUT_CYCLES(TIMEOUT_CYCLES)
    ) master (
        .clk(clk),
        .rst_n(rst_n),
        .start(rx_start || tx_start),
        .read_en(owner_tx ? tx_read : rx_read),
        .dev_addr(owner_tx ? SECOND_ADDR : FIRST_ADDR),
        .reg_addr(owner_tx ? tx_addr : rx_addr),
        .wr_data(owner_tx ? tx_data : rx_data),
        .rd_data(received),
        .done(done),
        .error(error),
        .busy(busy),
        .scl(scl),
        .sda(sda)
    );
endmodule
