`timescale 1ns / 1ps
// Byte-wide PCS above the Pango hardware 8b/10b encoder/decoder.
// Supports Clause 37, SGMII 1G/full-duplex detection, and idle-only partners.
// 网络字节层：解析对端配置/空闲序列并协商链路，把 MAC 帧转换为 PCS 控制符和数据字节。
// 8b/10b 编解码由 HSST IP 完成；本模块在网络时钟域运行。
module base_x_pcs #(
    parameter BREAK_CYCLES = 1250000, ACK_CYCLES = 1250000, WATCH_CYCLES = 125000000
) (
    input clk, rstn, synced,
    input [7:0] rxd,
    input rxk, rxerr,
    output reg [7:0] txd,
    output reg txk,
    output reg idle_disparity,
    input [7:0] mac_txd,
    input mac_valid,
    output mac_ready,
    output reg [7:0] mac_rxd,
    output reg mac_rxvalid, mac_rxerr,
    output link,
    output reg sgmii,
    output reg parallel_detect
);

    // an 是自动协商状态；timer 负责阶段等待，quiet 检测长时间没有有效对端活动。
    localparam BRK = 0, ABILITY = 1, ACK = 2, COMPLETE = 3, IDLE_WAIT = 4, UP = 5;
    reg [2:0] an;
    reg [27:0] timer, quiet;
    reg [15:0] previous;
    reg [2:0] match_count, idles;
    reg [2:0] rxstate;
    reg [7:0] cfglo;
    reg [15:0] cfg;
    reg cfg_event, idle_event;
    wire abilities_ok = sgmii ?
        (cfg[15] && cfg[12] && cfg[11:10] == 2'b10) : (cfg[5] && !cfg[13] && !cfg[12]);
    assign link = an == UP && synced;
    wire config_send = an <= COMPLETE;
    wire [15:0] config_word = an == BRK ?
        16'h0000 : ((sgmii ? 16'h0001 : 16'h0020) | ((an >= ACK) ? 16'h4000 : 16'h0000));

    // txstate：0 逗号/起始，1 空闲字节，2 配置标识，3/4 配置低/高字节，5 帧数据/结束，6 扩展符。
    reg [2:0] txstate;
    reg parity, ctype;
    reg [15:0] txconfig;
    // /S/ consumes the first preamble byte. Every subsequent MAC byte is data.
    assign mac_ready = link && ((txstate == 0 && !config_send) || txstate == 5);

    // 发送符号选择；txk=1 表示控制符，txk=0 表示普通数据字节。
    always @* begin
        txd = 8'hbc;
        txk = 1;
        idle_disparity = 0;
        case (txstate)
            0: if (!config_send && mac_valid && link) txd = 8'hfb;
            1: begin
                txd = 8'h50;
                txk = 0;
                idle_disparity = 1;
            end
            2: begin
                txd = ctype ? 8'h42 : 8'hb5;
                txk = 0;
            end
            3: begin
                txd = txconfig[7:0];
                txk = 0;
            end
            4: begin
                txd = txconfig[15:8];
                txk = 0;
            end
            5: if (mac_valid) begin
                txd = mac_txd;
                txk = 0;
            end else txd = 8'hfd;
            6: txd = 8'hf7;
        endcase
    end

    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            txstate <= 0;
            parity <= 0;
            ctype <= 0;
            txconfig <= 0;
        end else begin
            parity <= ~parity;
            case (txstate)
                0: if (config_send) begin
                    txconfig <= config_word;
                    txstate  <= 2;
                end else if (mac_valid && link) txstate <= 5;
                else txstate <= 1;
                1: txstate <= 0;
                2: txstate <= 3;
                3: txstate <= 4;
                4: begin
                    txstate <= 0;
                    ctype   <= ~ctype;
                end
                5: if (!mac_valid) txstate <= 6;
                6: if (parity) txstate <= 0;
                default: txstate <= 0;
            endcase
        end
    end

    // 接收状态：1 等待配置/空闲标识，2/3 拼接配置字，4 接收 MAC 帧。错误随帧传给 MAC。
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            rxstate <= 0;
            cfglo <= 0;
            cfg <= 0;
            cfg_event <= 0;
            idle_event <= 0;
            mac_rxvalid <= 0;
            mac_rxerr <= 0;
            mac_rxd <= 0;
        end else begin
            cfg_event   <= 0;
            idle_event  <= 0;
            mac_rxvalid <= 0;
            mac_rxerr   <= 0;
            if (!synced || rxerr) begin
                if (rxstate == 4) begin
                    mac_rxvalid <= 1;
                    mac_rxerr   <= 1;
                end
                rxstate <= 0;
            end else if (rxk) begin
                if (rxd == 8'hbc) rxstate <= 1;
                else if (rxd == 8'hfb) begin
                    rxstate <= 4;
                    mac_rxd <= 8'h55;
                    mac_rxvalid <= 1;
                end else begin
                    if (rxstate == 4 && rxd != 8'hfd) begin
                        mac_rxvalid <= 1;
                        mac_rxerr   <= 1;
                    end
                    rxstate <= 0;
                end
            end else
                case (rxstate)
                    1: if (rxd == 8'hb5 || rxd == 8'h42) rxstate <= 2;
                    else begin
                        if (rxd == 8'h50 || rxd == 8'hc5) idle_event <= 1;
                        rxstate <= 0;
                    end
                    2: begin
                        cfglo   <= rxd;
                        rxstate <= 3;
                    end
                    3: begin
                        cfg <= {rxd, cfglo};
                        cfg_event <= 1;
                        rxstate <= 0;
                    end
                    4: begin
                        mac_rxvalid <= 1;
                        mac_rxd <= rxd;
                    end
                endcase
        end
    end

    // 链路建立与失效检测。sgmii/parallel_detect 由对端实际配置或空闲序列决定。
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            an <= BRK;
            timer <= 0;
            quiet <= 0;
            previous <= 0;
            match_count <= 0;
            idles <= 0;
            sgmii <= 0;
            parallel_detect <= 0;
        end else if (!synced) begin
            an <= BRK;
            timer <= 0;
            quiet <= 0;
            match_count <= 0;
            idles <= 0;
            sgmii <= 0;
            parallel_detect <= 0;
        end else begin
            if (timer < WATCH_CYCLES) timer <= timer + 1'b1;
            if (cfg_event || idle_event || mac_rxvalid) quiet <= 0;
            else if (quiet < WATCH_CYCLES) quiet <= quiet + 1'b1;
            if (idle_event) begin
                if (idles < 7) idles <= idles + 1'b1;
            end
            if (cfg_event) begin
                previous <= cfg;
                idles <= 0;
                if ((cfg & 16'hbfff) == (previous & 16'hbfff)) begin
                    if (match_count < 7) match_count <= match_count + 1'b1;
                end else match_count <= 0;
                if (cfg[0]) sgmii <= 1;
            end
            case (an)
                BRK: if (timer >= BREAK_CYCLES) begin
                    an <= ABILITY;
                    timer <= 0;
                    match_count <= 0;
                    idles <= 0;
                end
                ABILITY: begin
                    if (cfg_event && match_count >= 2 && abilities_ok) begin
                        an <= ACK;
                        timer <= 0;
                        match_count <= 0;
                    end else if (timer >= ACK_CYCLES && idles >= 3) begin
                        an <= UP;
                        timer <= 0;
                        parallel_detect <= 1;
                    end
                end
                ACK: if (cfg_event && match_count >= 2 && cfg[14] && abilities_ok) begin
                    an <= COMPLETE;
                    timer <= 0;
                end else if (timer >= WATCH_CYCLES) begin
                    an <= BRK;
                    timer <= 0;
                end
                COMPLETE: if (timer >= ACK_CYCLES) begin
                    an <= IDLE_WAIT;
                    timer <= 0;
                    idles <= 0;
                end
                IDLE_WAIT: if (idles >= 3) begin
                    an <= UP;
                    timer <= 0;
                end else if (timer >= WATCH_CYCLES) begin
                    an <= BRK;
                    timer <= 0;
                end
                UP: if (quiet >= WATCH_CYCLES || (cfg_event && (!cfg[14] || !abilities_ok))) begin
                    an <= BRK;
                    timer <= 0;
                    parallel_detect <= 0;
                end
            endcase
        end
    end
endmodule
