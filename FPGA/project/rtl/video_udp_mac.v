`timescale 1ns / 1ps

// Direct-cable IPv4/UDP sender + validated ARP request/reply handling.
// Address parameters identify independent network interfaces.
// QV02 packet geometry is derived from IMAGE_BYTES; RGB565 is unchanged.
// 以太网帧构造与 ARP 接收：视频采用 IPv4/UDP，负载为 QV02 头加 RGB565 图像块。
// 每个输出字节只在 txready 握手时推进，视频队列在整帧 FCS 发完后释放当前包。
module video_udp_mac #(
    parameter CONTROL=0,DIAGNOSTICS=0,DIAG_INTERVAL=125000000,
    parameter ARP_INTERVAL = 31250000,
    parameter IMAGE_BYTES = 1440,
    parameter WORD_BITS = $clog2(IMAGE_BYTES / 4),
    parameter [47:0] LOCAL_MAC = 48'h025000001002,
    parameter [31:0] LOCAL_IP = 32'hc0a80a02, PEER_IP = 32'hc0a80a01
) (
    input [767:0] diagnostic_data,
    input clk, rstn, link,
    input [7:0] rxd,
    input rxvalid, rxerr,
    output reg [7:0] txd,
    output txvalid,
    input txready,
    input empty,
    input [31:0] frame_id, source_id,
    input [10:0] packet_id,
    input [15:0] stream_port,
    output reg release_packet,
    output reg [WORD_BITS-1:0] pixel_addr,
    input [31:0] pixel_data,
    output reg peer_known,
    output reg [31:0] sent_packets,
    output reg rx_fault,
    input applied_toggle,input [1:0] actual_mode,input [31:0] applied_seq,applied_frame,
    output mode_toggle,output [1:0] desired_mode,output [31:0] desired_seq
);

    // 8192 image bytes -> IP length 8248, requires end-to-end MTU >=8248.
    // Both 1440 and 8192 divide the full RGB565 frame, so no padded tail.

    // 长度单位均为字节：IP_LENGTH 含 IPv4/UDP/QV02；UDP_LENGTH 含 UDP/QV02。
    // 8192 图像字节 + 28 字节 QV02 + 8 字节 UDP + 20 字节 IPv4 = 8248 字节 IP 包。
    localparam [15:0] PACKETS_PER_FRAME = 1280 * 720 * 2 / IMAGE_BYTES;
    localparam [15:0] IP_LENGTH = IMAGE_BYTES + 56, UDP_LENGTH = IMAGE_BYTES + 36;
    localparam BYTE_BITS = $clog2(IMAGE_BYTES + 70);
    localparam DATA_START = 70;

    // Ethernet 14 + IPv4 20 + UDP 8 + QV02 28

    // 按字节更新以太网反射 CRC32；发送 FCS 时取反，接收时检查包含 FCS 的最终残值。
    function [31:0] crc_byte;
        input [31:0] c;
        input [7:0] d;
        reg [31:0] a;
        integer j;
        begin
            a = c;
            for (j = 0; j < 8; j = j + 1) begin
                if (a[0] ^ d[j]) a = (a >> 1) ^ 32'hedb88320;
                else a = a >> 1;
            end
            crc_byte = a;
        end
    endfunction

    function [15:0] ip_checksum;
        input unused;
        reg [31:0] sum;
        begin
            sum = 32'h4500 + IP_LENGTH + 16'h4000 + 16'h4011 + LOCAL_IP[31:16] + LOCAL_IP[15:0] +
                PEER_IP[31:16] + PEER_IP[15:0];
            sum = (sum & 65535) + (sum >> 16);
            sum = (sum & 65535) + (sum >> 16);
            ip_checksum = ~sum[15:0];
        end
    endfunction
    localparam [15:0] IPCSUM = ip_checksum(0);
    function [15:0] ack_checksum;input unused;reg [31:0] sum;
      begin sum=32'h4500+56+16'h4000+16'h4011+LOCAL_IP[31:16]+LOCAL_IP[15:0]+PEER_IP[31:16]+PEER_IP[15:0];sum=(sum&65535)+(sum>>16);sum=(sum&65535)+(sum>>16);ack_checksum=~sum[15:0];end
    endfunction
    localparam [15:0] ACKCSUM=ack_checksum(0);
    function [15:0] diag_checksum;input unused;reg [31:0] sum;
      begin sum=32'h4500+124+16'h4000+16'h4011+LOCAL_IP[31:16]+LOCAL_IP[15:0]+PEER_IP[31:16]+PEER_IP[15:0];sum=(sum&65535)+(sum>>16);sum=(sum&65535)+(sum>>16);diag_checksum=~sum[15:0];end
    endfunction
    localparam [15:0] DIAGCSUM=diag_checksum(0);

    function [7:0] mac_byte;
        input [47:0] address;
        input [2:0] n;
        begin
            case (n)
                0: mac_byte = address[47:40];
                1: mac_byte = address[39:32];
                2: mac_byte = address[31:24];
                3: mac_byte = address[23:16];
                4: mac_byte = address[15:8];
                5: mac_byte = address[7:0];
                default: mac_byte = 0;
            endcase
        end
    endfunction

    // 接收侧只处理 ARP；记录帧头字段、累计 CRC，并在帧结束时统一校验对端身份。
    reg rx_d, in_body, bad;
    reg [15:0] rx_byte_count;
    reg [31:0] rx_crc;
    reg [47:0] ether_dest, ether_src, arp_sha;
    reg [31:0] arp_spa, arp_tpa;
    reg [15:0] etype, arp_htype, arp_ptype, arp_op;
    reg [7:0] arp_hlen, arp_plen;
    reg [47:0] peer_mac, reply_mac;
    reg reply_pending;
    reg [31:0] arp_timer;

    // 发送状态：选择视频/ARP → 前导码 → 帧体 → 四字节 FCS → 帧间间隔。
    // packet_kind：0 视频，1 ARP 请求，2 ARP 应答。
    localparam TX_IDLE = 0, TX_PREAMBLE = 1, TX_BODY = 2, TX_FCS = 3, TX_IFG = 4;
    reg [2:0] tx_state;
    reg [BYTE_BITS-1:0] byte_index;
    reg [3:0] preamble_index, ifg_count;
    reg [1:0] fcs_index;reg [2:0] packet_kind;
    reg [31:0] diag_timer;reg diag_pending;reg [767:0] diag_hold;
    wire diagnostic_kind=packet_kind==4;
    reg [31:0] tx_crc, frame_hold;
    reg crc_pending,app_pending;
    reg [7:0] accepted_byte;
    wire [31:0] effective_crc=crc_pending ? crc_byte(tx_crc,accepted_byte) : tx_crc;
    reg [10:0] packet_hold;
    reg [47:0] destination;
    wire video_kind = packet_kind == 0;
    wire control_kind=packet_kind==3;
    wire command;wire [31:0] cmd_session,cmd_seq;wire [7:0] cmd_op,cmd_mode;
    wire ack_pending;wire [223:0] ack_data;reg [223:0] ack_hold;reg [31:0] ack_crc;
    wire [31:0] effective_ack_crc=crc_pending && app_pending ? crc_byte(ack_crc,accepted_byte) : ack_crc;
    wire ack_take=tx_state==TX_IDLE && link && !reply_pending && peer_known && ack_pending;
    control_rx #(.LOCAL_MAC(LOCAL_MAC),.LOCAL_IP(LOCAL_IP),.PEER_IP(PEER_IP)) control_input(
      .clk(clk),.rstn(rstn),.link(link && CONTROL),.known(peer_known),.peer_mac(peer_mac),.rxd(rxd),.valid(rxvalid),.err(rxerr),
      .request(command),.session(cmd_session),.seq(cmd_seq),.operation(cmd_op),.mode(cmd_mode));
    control_mode commands(.clk(clk),.rstn(rstn),.link(link),.request(command),.session(cmd_session),.seq(cmd_seq),.operation(cmd_op),.mode(cmd_mode),
      .applied_toggle(applied_toggle),.actual_mode(actual_mode),.applied_seq(applied_seq),.applied_frame(applied_frame),
      .mode_toggle(mode_toggle),.desired_mode(desired_mode),.desired_seq(desired_seq),.reply_take(ack_take),.reply_pending(ack_pending),.reply(ack_data));
    wire [BYTE_BITS-1:0] body_last = video_kind ? (IMAGE_BYTES + 69) : control_kind ? 69 : diagnostic_kind ? 137 : 59;
    wire start_reply = tx_state == TX_IDLE && link && reply_pending;
    wire [31:0] offset = packet_hold * IMAGE_BYTES;
    reg [1:0] byte_phase;
    reg [WORD_BITS-1:0] ram_word_index;
    reg [31:0] payload_word;
    reg [31:0] source_hold;
    reg [15:0] port_hold;

    // Prefetch the next word on byte 2, latch it after byte 3. This register
    // breaks the RAM -> channel/byte mux -> CRC path at the 125MHz MAC clock.
    // Registered prefetch: phase1 acceptance starts word N+1 for phase2.
    // Match the original active-payload address on every cycle, including stalls.
    // IDLE resets to word0 well before the next packet payload starts.
    always @(posedge clk or negedge rstn) begin
        if (!rstn) pixel_addr <= 0;
        else if (tx_state == TX_IDLE || !link) pixel_addr <= 0;
        else if (tx_state == TX_BODY && video_kind && txready &&
                 byte_index >= DATA_START && byte_phase == 1)
            pixel_addr <= ram_word_index + 1'b1;
    end
    assign txvalid = tx_state == TX_PREAMBLE || tx_state == TX_BODY || tx_state == TX_FCS;
    reg [7:0] body_byte;

    // 帧体字节表：0..13 以太网头，14..33 IPv4，34..41 UDP，42..69 QV02，70 起为图像。
    // QV02 的多字节字段按网络字节序发送；RGB565 图像保持像素低字节在前。
    always @* begin
        body_byte = 0;
        if (byte_index < 6) body_byte = mac_byte(destination, byte_index[2:0]);
        else if (byte_index < 12) body_byte = mac_byte(LOCAL_MAC, byte_index[2:0] - 3'd6);
        else if (video_kind || control_kind || diagnostic_kind) begin
            case (byte_index)
                12: body_byte = 8'h08;
                13: body_byte = 0;
                14: body_byte = 8'h45;
                15: body_byte = 0;
                16: body_byte = IP_LENGTH[15:8];
                17: body_byte = IP_LENGTH[7:0];
                18, 19: body_byte = 0;
                20: body_byte = 8'h40;
                21: body_byte = 0;
                22: body_byte = 8'h40;
                23: body_byte = 8'h11;
                24: body_byte = IPCSUM[15:8];
                25: body_byte = IPCSUM[7:0];
                26: body_byte = LOCAL_IP[31:24];
                27: body_byte = LOCAL_IP[23:16];
                28: body_byte = LOCAL_IP[15:8];
                29: body_byte = LOCAL_IP[7:0];
                30: body_byte = PEER_IP[31:24];
                31: body_byte = PEER_IP[23:16];
                32: body_byte = PEER_IP[15:8];
                33: body_byte = PEER_IP[7:0];
                34, 36: body_byte = port_hold[15:8];
                35, 37: body_byte = port_hold[7:0];
                38: body_byte = UDP_LENGTH[15:8];
                39: body_byte = UDP_LENGTH[7:0];
                40, 41: body_byte = 0;

                // UDP checksum omitted on IPv4; IP DF remains set.
                42: body_byte = "Q";
                43: body_byte = "V";
                44: body_byte = "0";
                45: body_byte = port_hold==5200 ? "4" : port_hold==5100 ? "3" : "2";
                46: body_byte = frame_hold[31:24];
                47: body_byte = frame_hold[23:16];
                48: body_byte = frame_hold[15:8];
                49: body_byte = frame_hold[7:0];
                50: body_byte = {5'b0, packet_hold[10:8]};
                51: body_byte = packet_hold[7:0];
                52: body_byte = PACKETS_PER_FRAME[15:8];
                53: body_byte = PACKETS_PER_FRAME[7:0];

                // Width 1280
                54: body_byte = port_hold==5200 ? 8'h0a : 8'h05;
                55: body_byte = 0;

                // Height 720
                56: body_byte = port_hold==5200 ? 8'h01 : 8'h02;
                57: body_byte = port_hold==5200 ? 8'h68 : 8'hd0;

                // Byte offset within the complete RGB565 frame
                58: body_byte = offset[31:24];
                59: body_byte = offset[23:16];
                60: body_byte = offset[15:8];
                61: body_byte = offset[7:0];
                62: body_byte = IMAGE_BYTES[15:8];
                63: body_byte = IMAGE_BYTES[7:0];

                // RGB565 pixel format, no padding bytes
                64: body_byte = 0;
                65: body_byte = 1;

                // format 1: unchanged RGB565, little endian
                66: body_byte = source_hold[31:24];
                67: body_byte = source_hold[23:16];
                68: body_byte = source_hold[15:8];
                69: body_byte = source_hold[7:0];
                default: body_byte = payload_word[byte_phase*8+:8];
            endcase
        end else begin
            case (byte_index)
                12: body_byte = 8'h08;
                13: body_byte = 8'h06;
                14: body_byte = 0;
                15: body_byte = 1;
                16: body_byte = 8'h08;
                17: body_byte = 0;
                18: body_byte = 6;
                19: body_byte = 4;
                20: body_byte = 0;
                21: body_byte = packet_kind == 2 ? 2 : 1;
                28: body_byte = LOCAL_IP[31:24];
                29: body_byte = LOCAL_IP[23:16];
                30: body_byte = LOCAL_IP[15:8];
                31: body_byte = LOCAL_IP[7:0];
                38: body_byte = PEER_IP[31:24];
                39: body_byte = PEER_IP[23:16];
                40: body_byte = PEER_IP[15:8];
                41: body_byte = PEER_IP[7:0];
                default: begin
                    if (byte_index >= 22 && byte_index <= 27)
                        body_byte = mac_byte(LOCAL_MAC, byte_index[2:0] - 3'd6);
                    else if (byte_index >= 32 && byte_index <= 37 && packet_kind == 2)
                        body_byte = mac_byte(destination, byte_index[2:0]);
                end
            endcase
        end
        if(control_kind && byte_index>=14)begin
            case(byte_index)
              16:body_byte=0;17:body_byte=56;24:body_byte=ACKCSUM[15:8];25:body_byte=ACKCSUM[7:0];
              34:body_byte=8'h17;35:body_byte=8'hd4;36:body_byte=8'h17;37:body_byte=8'hd5;
              38:body_byte=0;39:body_byte=36;
              default:if(byte_index>=42 && byte_index<=65)body_byte=ack_hold[(69-byte_index)*8+:8];
                      else if(byte_index>=66)case(byte_index)
                        66:body_byte=~effective_ack_crc[31:24];67:body_byte=~effective_ack_crc[23:16];68:body_byte=~effective_ack_crc[15:8];69:body_byte=~effective_ack_crc[7:0];endcase
            endcase
        end
        if(diagnostic_kind && byte_index>=14)begin
            case(byte_index)
              16:body_byte=0;17:body_byte=124;24:body_byte=DIAGCSUM[15:8];25:body_byte=DIAGCSUM[7:0];
              34,36:body_byte=8'h18;35,37:body_byte=8'h37;
              38:body_byte=0;39:body_byte=104;
              default:if(byte_index>=42)body_byte=diag_hold[767:760];
            endcase
        end
        txd = 0;
        case (tx_state)
            TX_PREAMBLE: txd = preamble_index == 7 ? 8'hd5 : 8'h55;
            TX_BODY: txd = body_byte;
            TX_FCS: txd = ~effective_crc[7:0];
        endcase
    end

    // 接收时序：从 SFD 开始累计以太网帧；只有合法 ARP 帧才更新 peer_mac/peer_known。
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            rx_d <= 0;
            in_body <= 0;
            bad <= 0;
            rx_byte_count <= 0;
            rx_crc <= 32'hffffffff;
            ether_dest <= 0;
            ether_src <= 0;
            arp_sha <= 0;
            arp_spa <= 0;
            arp_tpa <= 0;
            etype <= 0;
            arp_htype <= 0;
            arp_ptype <= 0;
            arp_op <= 0;
            arp_hlen <= 0;
            arp_plen <= 0;
            peer_known <= 0;
            peer_mac <= 0;
            reply_mac <= 0;
            reply_pending <= 0;
            rx_fault <= 0;
        end else begin
            rx_d <= rxvalid;
            if (start_reply) reply_pending <= 0;
            if (!link) begin
                peer_known <= 0;
                reply_pending <= 0;
            end
            if (rxvalid) begin
                if (rxerr) bad <= 1;
                if (!in_body) begin
                    if (rxd == 8'hd5) begin
                        in_body <= 1;
                        rx_byte_count <= 0;
                        rx_crc <= 32'hffffffff;
                        bad <= rxerr;
                    end
                end else begin
                    rx_crc <= crc_byte(rx_crc, rxd);
                    if (rx_byte_count != 65535) rx_byte_count <= rx_byte_count + 1'b1;
                    if (rx_byte_count < 6) ether_dest <= {ether_dest[39:0], rxd};
                    if (rx_byte_count >= 6 && rx_byte_count < 12)
                        ether_src <= {ether_src[39:0], rxd};
                    case (rx_byte_count)
                        12, 13: etype <= {etype[7:0], rxd};
                        14, 15: arp_htype <= {arp_htype[7:0], rxd};
                        16, 17: arp_ptype <= {arp_ptype[7:0], rxd};
                        18: arp_hlen <= rxd;
                        19: arp_plen <= rxd;
                        20, 21: arp_op <= {arp_op[7:0], rxd};
                    endcase
                    if (rx_byte_count >= 22 && rx_byte_count < 28) arp_sha <= {arp_sha[39:0], rxd};
                    if (rx_byte_count >= 28 && rx_byte_count < 32) arp_spa <= {arp_spa[23:0], rxd};
                    if (rx_byte_count >= 38 && rx_byte_count < 42) arp_tpa <= {arp_tpa[23:0], rxd};
                end
            end else if (rx_d) begin
                in_body <= 0;
                bad <= 0;
                if (in_body && (bad || rx_crc != 32'hdebb20e3 || rx_byte_count < 64)) rx_fault <= 1;
                if (link && in_body && !bad && rx_byte_count >= 64 && rx_crc == 32'hdebb20e3 &&
                    (ether_dest == LOCAL_MAC || ether_dest == 48'hffffffffffff) && etype == 16'h0806
                    && arp_htype == 1 && arp_ptype == 16'h0800 && arp_hlen == 6 && arp_plen == 4 &&
                    (arp_op == 1 || arp_op == 2) && arp_spa == PEER_IP && arp_tpa == LOCAL_IP &&
                    arp_sha == ether_src && !arp_sha[40] && arp_sha != 0) begin
                    peer_mac   <= arp_sha;
                    peer_known <= 1;
                    if (arp_op == 1) begin
                        reply_pending <= 1;
                        reply_mac <= arp_sha;
                    end
                end
            end
        end
    end

    // 发送时序：ARP 应答优先，其次为未知对端时的 ARP 请求，最后发送排队视频。
    always @(posedge clk or negedge rstn) begin
        if (!rstn) begin
            tx_state <= TX_IDLE;
            byte_index <= 0;
            preamble_index <= 0;
            ifg_count <= 0;
            fcs_index <= 0;
            diag_timer<=0;diag_pending<=0;diag_hold<=0;
            packet_kind <= 0;ack_hold<=0;ack_crc<=32'hffffffff;
            tx_crc <= 32'hffffffff;crc_pending<=0;app_pending<=0;accepted_byte<=0;
            payload_word <= 0;
            byte_phase <= 0;
            ram_word_index <= 0;
            source_hold <= 0;
            port_hold <= 0;
            frame_hold <= 0;
            packet_hold <= 0;
            destination <= 0;
            release_packet <= 0;
            sent_packets <= 0;
            arp_timer <= ARP_INTERVAL;
        end else begin
            release_packet <= 0;
            crc_pending<=0;app_pending<=0;
            if(crc_pending)tx_crc<=effective_crc;
            if(crc_pending && app_pending)ack_crc<=effective_ack_crc;
            if(DIAGNOSTICS)begin
                if(diag_timer==DIAG_INTERVAL-1)begin diag_timer<=0;diag_pending<=1;end
                else diag_timer<=diag_timer+1'b1;
            end
            if (arp_timer < ARP_INTERVAL) arp_timer <= arp_timer + 1'b1;
            if (!link) begin
                if (tx_state != TX_IDLE && tx_state != TX_IFG && video_kind) release_packet <= 1;
                tx_state  <= TX_IDLE;
                arp_timer <= ARP_INTERVAL;
            end else
                case (tx_state)
                    TX_IDLE: begin
                        byte_phase <= 0;
                        ram_word_index <= 0;
                        byte_index <= 0;
                        preamble_index <= 0;
                        tx_crc <= 32'hffffffff;ack_crc<=32'hffffffff;
                        if (reply_pending) begin
                            packet_kind <= 2;
                            destination <= reply_mac;
                            tx_state <= TX_PREAMBLE;
                        end else if (ack_pending && peer_known) begin
                            packet_kind<=3;destination<=peer_mac;ack_hold<=ack_data;tx_state<=TX_PREAMBLE;
                        end else if (DIAGNOSTICS && diag_pending && peer_known)begin
                            packet_kind<=4;destination<=peer_mac;diag_hold<=diagnostic_data;diag_pending<=0;tx_state<=TX_PREAMBLE;
                        end else if (!peer_known && arp_timer >= ARP_INTERVAL) begin
                            packet_kind <= 1;
                            destination <= 48'hffffffffffff;
                            tx_state <= TX_PREAMBLE;
                            arp_timer <= 0;
                        end else if (peer_known && !empty) begin
                            packet_kind <= 0;
                            destination <= peer_mac;
                            frame_hold <= frame_id;
                            packet_hold <= packet_id;
                            source_hold <= source_id;
                            port_hold <= stream_port;
                            tx_state <= TX_PREAMBLE;
                        end
                    end
                    TX_PREAMBLE: if (txready) begin
                        if (preamble_index == 7) begin
                            tx_state   <= TX_BODY;
                            byte_index <= 0;
                        end else preamble_index <= preamble_index + 1'b1;
                    end

                    // txready 停顿时不推进帧体、CRC 或字节相位；RAM 预取保持原有两拍使用节奏。
                    TX_BODY: if (txready) begin
                        if(diagnostic_kind && byte_index>=42)diag_hold<={diag_hold[759:0],8'd0};
                        if (video_kind && (byte_index == DATA_START - 1 ||
                                           (byte_index >= DATA_START && byte_phase == 3)))
                            payload_word <= pixel_data;
                        if (video_kind && byte_index >= DATA_START) begin
                            if (byte_phase == 3) begin
                                byte_phase <= 0;
                                ram_word_index <= ram_word_index + 1'b1;
                            end else byte_phase <= byte_phase + 1'b1;
                        end
                        accepted_byte<=txd;crc_pending<=1;
                        app_pending<=control_kind && byte_index>=42 && byte_index<=65;
                        if (byte_index == body_last) begin
                            tx_state  <= TX_FCS;
                            fcs_index <= 0;
                        end else byte_index <= byte_index + 1'b1;
                    end

                    // 第四个 FCS 字节握手后才释放视频包；ARP 帧不会推进视频队列。
                    TX_FCS: if (txready) begin
                        tx_crc <= {8'hff, effective_crc[31:8]};
                        if (fcs_index == 3) begin
                            tx_state  <= TX_IFG;
                            ifg_count <= 0;
                            if (video_kind) begin
                                release_packet <= 1;
                                sent_packets   <= sent_packets + 1'b1;
                            end
                        end else fcs_index <= fcs_index + 1'b1;
                    end
                    TX_IFG: if (ifg_count == 15) tx_state <= TX_IDLE;
                    else ifg_count <= ifg_count + 1'b1;
                    default: tx_state <= TX_IDLE;
                endcase
        end
    end
endmodule
