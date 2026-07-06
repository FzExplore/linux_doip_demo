#include "doip_server.h"
#include "uds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>

// ============================================================
//  工具函数：打印十六进制数据
// ============================================================
static void print_hex(const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

// ============================================================
//  初始化服务器：创建 UDP 和 TCP 监听 socket
// ============================================================
int doip_server_init(doip_server_t *srv) {
    memset(srv, 0, sizeof(*srv));
    FD_ZERO(&srv->master_set);

    // 确保 data 目录存在（存储持久化数据）
    mkdir("./data", 0755);
    mkdir("./data/ota", 0755);
    mkdir("./data/ota/staging", 0755);
    mkdir("./data/ota/slot_a", 0755);
    mkdir("./data/ota/slot_b", 0755);
    mkdir("./data/ota/vendor", 0755);

    // --- 设置模拟车辆信息 ---
    memcpy(srv->vin, "DEMODEVICE1234567", 17);
    srv->logical_address = 0x0E80;
    memset(srv->eid, 0xAB, 6);
    memset(srv->gid, 0xCD, 6);

    // --- 创建 TCP 监听 socket ---
    srv->tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->tcp_listen_fd < 0) {
        perror("TCP socket");
        return -1;
    }

    int opt = 1;
    setsockopt(srv->tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));//确保无60s的TIME_WAIT状态，否则会报错

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;          // 监听所有网卡
    addr.sin_port        = htons(DOIP_TCP_DATA_PORT);

    if (bind(srv->tcp_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("TCP bind");
        return -1;
    }

    if (listen(srv->tcp_listen_fd, MAX_TCP_CLIENTS) < 0) {
        perror("TCP listen");
        return -1;
    }

    FD_SET(srv->tcp_listen_fd, &srv->master_set);
    srv->max_fd = srv->tcp_listen_fd;

    // --- 创建 UDP 监听 socket ---
    srv->udp_listen_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv->udp_listen_fd < 0) {
        perror("UDP socket");
        return -1;
    }

    setsockopt(srv->udp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family      = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port        = htons(DOIP_UDP_DISCOVERY_PORT);

    if (bind(srv->udp_listen_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("UDP bind");
        return -1;
    }

    FD_SET(srv->udp_listen_fd, &srv->master_set);
    if (srv->udp_listen_fd > srv->max_fd)
        srv->max_fd = srv->udp_listen_fd;

    printf("[SERVER] 初始化完成\n");
    printf("  TCP 监听 : 0.0.0.0:%d\n", DOIP_TCP_DATA_PORT);
    printf("  UDP 监听 : 0.0.0.0:%d\n", DOIP_UDP_DISCOVERY_PORT);
    printf("  VIN      : %.17s\n", srv->vin);
    printf("  逻辑地址  : 0x%04X\n", srv->logical_address);

    // 加载持久化 DID（软件版本等可写数据）
    uds_init();

    return 0;
}

// ============================================================
//  接收完整的 DoIP 消息
// ============================================================
static int recv_doip_message(int fd, doip_header_t *header,
                             uint8_t *payload, uint32_t payload_buf_size) {
    // 先收 8 字节头部
    uint8_t header_buf[8];
    ssize_t n = recv(fd, header_buf, 8, MSG_WAITALL);
    if (n != 8) return -1;

    memcpy(header, header_buf, 8);

    // 网络字节序转本机字节序
    header->payload_type   = ntohs(header->payload_type);
    header->payload_length = ntohl(header->payload_length);

    // 校验协议版本
    if (header->protocol_version != DOIP_PROTOCOL_VERSION ||
        header->inverse_version  != (uint8_t)(~DOIP_PROTOCOL_VERSION)) {
        printf("[ERROR] 协议版本不匹配: ver=0x%02X inv=0x%02X\n",
               header->protocol_version, header->inverse_version);
        return -1;
    }

    // 再收载荷
    if (header->payload_length > 0) {
        if (header->payload_length > payload_buf_size) {
            printf("[ERROR] 载荷过长: %u 字节，丢弃报文\n",
                   header->payload_length);
            // 读掉多余字节，不关闭连接
            recv(fd, payload, payload_buf_size, MSG_WAITALL);
            return -2;  // -2 表示丢弃报文，不关闭 socket
        }
        n = recv(fd, payload, header->payload_length, MSG_WAITALL);
        if ((uint32_t)n != header->payload_length) return -1;
    }

    return 0;
}

// ============================================================
//  发送 DoIP 消息
// ============================================================
static int send_doip_message(int fd, uint16_t payload_type,
                             const uint8_t *payload, uint32_t payload_len) {
    doip_header_t header;
    header.protocol_version = DOIP_PROTOCOL_VERSION;
    header.inverse_version  = (uint8_t)(~DOIP_PROTOCOL_VERSION);
    header.payload_type     = htons(payload_type);
    header.payload_length   = htonl(payload_len);

    // 先发头部
    ssize_t n = send(fd, &header, sizeof(header), 0);
    if (n != sizeof(header)) return -1;

    // 再发载荷
    if (payload_len > 0 && payload != NULL) {
        n = send(fd, payload, payload_len, 0);
        if ((uint32_t)n != payload_len) return -1;
    }

    return 0;
}

// ============================================================
//  发送 DoIP 消息（UDP 专用，需指定目标地址）
// ============================================================
static int send_doip_udp_message(int fd, uint16_t payload_type,
                                 const uint8_t *payload, uint32_t payload_len,
                                 struct sockaddr_in *dest, socklen_t dest_len) {
    // UDP 是数据报，必须头+载荷合并成一次 sendto()，不能分开发
    uint8_t pkt[2048];
    uint32_t total = 8 + payload_len;
    if (total > sizeof(pkt)) return -1;

    pkt[0] = DOIP_PROTOCOL_VERSION;
    pkt[1] = (uint8_t)(~DOIP_PROTOCOL_VERSION);
    pkt[2] = (payload_type >> 8) & 0xFF;
    pkt[3] = (payload_type >> 0) & 0xFF;
    pkt[4] = (payload_len >> 24) & 0xFF;
    pkt[5] = (payload_len >> 16) & 0xFF;
    pkt[6] = (payload_len >> 8) & 0xFF;
    pkt[7] = (payload_len >> 0) & 0xFF;

    if (payload_len > 0 && payload != NULL)
        memcpy(pkt + 8, payload, payload_len);

    ssize_t n = sendto(fd, pkt, total, 0,
                       (struct sockaddr *)dest, dest_len);
    return ((uint32_t)n == total) ? 0 : -1;
}

// ============================================================
//  处理 UDP 消息（车辆发现）
// ============================================================
static void handle_udp(doip_server_t *srv) {
    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);
    uint8_t buf[RECV_BUFFER_SIZE];

    ssize_t n = recvfrom(srv->udp_listen_fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from_addr, &addr_len);
    if (n < 8) return;   // DoIP 头至少 8 字节

    doip_header_t *header = (doip_header_t *)buf;
    uint8_t *payload = buf + 8;

    // 收到的 UDP 直接从buf读，也要做字节序转换
    header->payload_type = ntohs(header->payload_type);

    printf("\n[UDP] 收到消息，来自 %s:%d, 类型=0x%04X\n",
           inet_ntoa(from_addr.sin_addr),
           ntohs(from_addr.sin_port),
           header->payload_type);

    switch (header->payload_type) {

    case DOIP_VEHICLE_ID_REQ:
    case DOIP_VEHICLE_ID_REQ_EID:
    case DOIP_VEHICLE_ID_REQ_VIN: {
        // 构造车辆识别响应
        doip_vehicle_id_resp_t resp;
        memcpy(resp.vin, srv->vin, 17);
        resp.logical_address = htons(srv->logical_address);
        memcpy(resp.eid, srv->eid, 6);
        memcpy(resp.gid, srv->gid, 6);
        resp.further_action = 0x00;   // 无进一步动作

        send_doip_udp_message(srv->udp_listen_fd,
                              DOIP_VEHICLE_ANNOUNCE,
                              (uint8_t *)&resp, sizeof(resp),
                              &from_addr, sizeof(from_addr));

        printf("[UDP] 已回复车辆识别响应 (len=%zu)\n", sizeof(resp));
        break;
    }

    default:
        printf("[UDP] 不支持的类型 0x%04X，忽略\n",
               header->payload_type);
        break;
    }
}

// ============================================================
//  处理 TCP 消息（诊断通信）
// ============================================================
static void handle_tcp_client(doip_server_t *srv, int idx) {
    doip_client_t *cli = &srv->clients[idx];
    doip_header_t header;
    uint8_t payload[RECV_BUFFER_SIZE];

    int ret = recv_doip_message(cli->sock_fd, &header,
                          payload, sizeof(payload));
    if (ret < 0) {
        if (ret == -2) {
            // 载荷过长，丢弃报文，不关闭连接
            printf("[TCP] 客户端 #%d 丢弃超长报文，继续\n", idx);
            return;
        }
        // -1: 连接断了或协议版本错，关闭连接
        printf("[TCP] 客户端 #%d 断开连接\n", idx);
        close(cli->sock_fd);
        FD_CLR(cli->sock_fd, &srv->master_set);
        cli->sock_fd = -1;
        cli->state   = CLIENT_FREE;
        return;
    }

    uint16_t type = header.payload_type;

    // 更新最后活动时间
    cli->last_activity = time(NULL);

    printf("[TCP] 客户端 #%d 收到消息, 类型=0x%04X, 载荷=%u 字节\n",
           idx, type, header.payload_length);

    // --- 根据消息类型分发处理 ---
    switch (type) {

case DOIP_ROUTING_ACTIVE_REQ: {
    // ============================================================
    //  a) 未知源地址 → 0x00
    //  b) 激活类型无效 → 0x06
    //  c1) 所有 socket 已激活 → 0x01
    //  c2) SA 与连接表不一致 → 0x02
    //  c3) SA 已在其他 socket 注册 → 0x03
    //  d) 需要认证 → 0x04
    //  成功 → 0x10
    // ============================================================

    // 报文长度：标准 7 字节，兼容 4 字节短格式
    if (header.payload_length < 4 || header.payload_length > 7) {
        printf("[TCP] 丢弃路由激活: 载荷长度无效 (%u)\n",
               header.payload_length);
        break;
    }

    doip_routing_active_req_t *req = (doip_routing_active_req_t *)payload;
    uint16_t tester_addr = ntohs(req->source_address);
    uint8_t  act_type    = req->activation_type;

    // 辅助宏：构造并发送路由激活响应后关闭连接
    #define SEND_ROUTING_RESP_AND_CLOSE(code) do { \
        doip_routing_active_resp_t _r; \
        _r.tester_logical_address = htons(tester_addr); \
        _r.target_logical_address = htons(srv->logical_address); \
        _r.response_code = (code); \
        memset(_r.reserved, 0, sizeof(_r.reserved)); \
        send_doip_message(cli->sock_fd, DOIP_ROUTING_ACTIVE_RESP, \
                          (uint8_t *)&_r, sizeof(_r)); \
    } while(0)

    // ---- a) 未知源地址 → 0x00 ----
    static const uint16_t whitelist[] = { 0x0E00, 0x0E80, 0x0F00 };
    int known = 0;
    for (size_t i = 0; i < sizeof(whitelist) / sizeof(whitelist[0]); i++) {
        if (tester_addr == whitelist[i]) { known = 1; break; }
    }
    if (!known) {
        SEND_ROUTING_RESP_AND_CLOSE(ROUTING_UNKNOWN_SRC);
        printf("[TCP] 路由激活-拒绝: 未知源地址 0x%04X (0x00)\n", tester_addr);
        close(cli->sock_fd);
        FD_CLR(cli->sock_fd, &srv->master_set);
        cli->sock_fd = -1;
        cli->state   = CLIENT_FREE;
        break;
    }

    // ---- b) 激活类型无效 → 0x06 ----
    // 0x00 = 默认, 0x01 = WWH-OBD, 其余保留
    if (act_type != 0x00 && act_type != 0x01) {
        SEND_ROUTING_RESP_AND_CLOSE(ROUTING_INVALID_ACT_TYPE);
        printf("[TCP] 路由激活-拒绝: 无效激活类型 0x%02X (0x06)\n", act_type);
        close(cli->sock_fd);
        FD_CLR(cli->sock_fd, &srv->master_set);
        cli->sock_fd = -1;
        cli->state   = CLIENT_FREE;
        break;
    }

    // ---- c1) 所有 socket 已激活 → 0x01 ----
    {
        int all_active = 1;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (srv->clients[i].state == CLIENT_FREE) {
                all_active = 0;
                break;
            }
        }
        if (all_active) {
            SEND_ROUTING_RESP_AND_CLOSE(ROUTING_NO_SOCKET);
            printf("[TCP] 路由激活-拒绝: 所有 socket 已激活 (0x01)\n");
            close(cli->sock_fd);
            FD_CLR(cli->sock_fd, &srv->master_set);
            cli->sock_fd = -1;
            cli->state   = CLIENT_FREE;
            break;
        }
    }

    // ---- c2) 当前连接已激活且 SA 不一致 → 0x02 ----
    if (cli->state == CLIENT_ACTIVATED && cli->tester_addr != tester_addr) {
        SEND_ROUTING_RESP_AND_CLOSE(ROUTING_SA_DIFFERENT);
        printf("[TCP] 路由激活-拒绝: SA 0x%04X 与已激活 0x%04X 不一致 (0x02)\n",
               tester_addr, cli->tester_addr);
        close(cli->sock_fd);
        FD_CLR(cli->sock_fd, &srv->master_set);
        cli->sock_fd = -1;
        cli->state   = CLIENT_FREE;
        break;
    }

    // ---- c3) SA 已在其他 socket 注册 → 0x03 ----
    {
        int sa_conflict = 0;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (i != idx &&
                srv->clients[i].state == CLIENT_ACTIVATED &&
                srv->clients[i].tester_addr == tester_addr) {
                sa_conflict = 1;
                break;
            }
        }
        if (sa_conflict) {
            SEND_ROUTING_RESP_AND_CLOSE(ROUTING_SA_ON_OTHER_SOCKET);
            printf("[TCP] 路由激活-拒绝: SA 0x%04X 已在其他 socket 注册 (0x03)\n",
                   tester_addr);
            close(cli->sock_fd);
            FD_CLR(cli->sock_fd, &srv->master_set);
            cli->sock_fd = -1;
            cli->state   = CLIENT_FREE;
            break;
        }
    }

    // ---- d) 认证（暂不实现）----
    // 如需认证且未完成 → 0x04

    // ---- 全部通过：激活成功 → 0x10 ----
    {
        doip_routing_active_resp_t resp;
        resp.tester_logical_address = htons(tester_addr);
        resp.target_logical_address = htons(srv->logical_address);
        resp.response_code = ROUTING_SUCCESS;
        memset(resp.reserved, 0, sizeof(resp.reserved));
        send_doip_message(cli->sock_fd, DOIP_ROUTING_ACTIVE_RESP,
                          (uint8_t *)&resp, sizeof(resp));

        cli->tester_addr = tester_addr;
        cli->state = CLIENT_ACTIVATED;
        printf("[TCP] 路由已激活, 测试仪=0x%04X, 类型=0x%02X\n",
               tester_addr, act_type);
    }
    break;
}

    case DOIP_DIAG_MESSAGE: {
        if (cli->state != CLIENT_ACTIVATED) {
            printf("[TCP] 拒绝：路由未激活\n");
            break;
        }
        doip_diag_message_t *diag = (doip_diag_message_t *)payload;
        diag->source_address = ntohs(diag->source_address);
        diag->target_address = ntohs(diag->target_address);
        uint32_t diag_data_len = header.payload_length - 4;

        printf("[TCP] 收到诊断消息: 源=0x%04X 目标=0x%04X 数据=%u字节\n",
               diag->source_address,
               diag->target_address,
               diag_data_len);
        print_hex(diag->user_data, diag_data_len);

        // --- 调用 UDS 引擎处理 ---
        static uds_context_t uds_ctx = { .current_session = DIAG_SESSION_DEFAULT };
        uint8_t  uds_resp[4096];
        uint32_t uds_resp_len = 0;

        int ret = uds_process_message(diag->user_data, diag_data_len,
                                      uds_resp, &uds_resp_len, &uds_ctx);
        if (ret == 0 && uds_resp_len > 0) {
            // ============================================================
            //  功能寻址抑制否定应答 — ISO 14229-1
            //  功能寻址（目标 >= 0xE000）时，否定应答（0x7F）不能回复，
            //  否则多个 ECU 同时回复会淹没诊断仪总线
            // ============================================================
            int is_functional = IS_FUNCTIONAL_ADDR(diag->target_address);
            int is_neg = (uds_resp[0] == UDS_NEG_RESP_SID);

            if (is_functional && is_neg) {
                printf("[TCP] 功能寻址-抑制否定应答 (NRC=0x%02X)\n", uds_resp[2]);
                break;
            }

            // 构造 DoIP 诊断响应：源地址 + 目标地址 + UDS 数据
            uint8_t doip_payload[4096];
            uint16_t *src = (uint16_t *)&doip_payload[0];
            uint16_t *dst = (uint16_t *)&doip_payload[2];
            *src = htons(srv->logical_address);   // 我发出去，源 = 本ECU
            *dst = htons(cli->tester_addr);        // 目标 = 测试仪
            memcpy(doip_payload + 4, uds_resp, uds_resp_len);

            send_doip_message(cli->sock_fd,
                              DOIP_DIAG_MESSAGE,
                              doip_payload, 4 + uds_resp_len);

            printf("[TCP] 已回复诊断响应 (%u 字节)\n", 4 + uds_resp_len);

            // 检查复位标志
            if (uds_ctx.reset_pending == 1) {
                printf("[SERVER] 执行硬复位 — 退出进程，由 Boot Manager 重启\n");
                close(cli->sock_fd);
                close(srv->tcp_listen_fd);
                close(srv->udp_listen_fd);
                exit(0);
            }
            if (uds_ctx.reset_pending == 2) {
                printf("[SERVER] 执行软复位 — 重置 UDS 状态\n");
                uds_ctx.reset_pending = 0;
            }
        }
        break;
    }

    case DOIP_ALIVE_CHECK_REQ: {
        // 存活检查：直接回复
        send_doip_message(cli->sock_fd,
                          DOIP_ALIVE_CHECK_RESP, NULL, 0);
        printf("[TCP] 回复存活检查\n");
        break;
    }

    case DOIP_ALIVE_CHECK_RESP: {
        // 诊断仪回复了我们的心跳，更新活动时间后正常忽略
        break;
    }

    default:
        printf("[TCP] 不支持的类型 0x%04X\n", type);
        break;
    }
}

// ============================================================
//  接受新的 TCP 连接
// ============================================================
static void accept_tcp_client(doip_server_t *srv) {
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);

    int client_fd = accept(srv->tcp_listen_fd,
                           (struct sockaddr *)&client_addr, &len);
    if (client_fd < 0) return;

    // 分配一个空闲的客户端槽位
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (srv->clients[i].state == CLIENT_FREE) {
            srv->clients[i].sock_fd    = client_fd;
            srv->clients[i].state      = CLIENT_CONNECTED;
            srv->clients[i].recv_len   = 0;
            srv->clients[i].last_activity = time(NULL);  // 初始化时间戳

            FD_SET(client_fd, &srv->master_set);
            if (client_fd > srv->max_fd)
                srv->max_fd = client_fd;

            printf("[TCP] 新连接 #%d 来自 %s:%d\n",
                   i,
                   inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port));
            return;
        }
    }

    // 没有空闲槽位，拒绝连接
    printf("[TCP] 连接数已满，拒绝 %s:%d\n",
           inet_ntoa(client_addr.sin_addr),
           ntohs(client_addr.sin_port));
    close(client_fd);
}

// ============================================================
//  主事件循环，select 监听所有 fd，UDP 和 TCP 一起管
// ============================================================
void doip_server_run(doip_server_t *srv) {
    printf("\n[SERVER] 开始运行，等待连接...\n\n");

    while (srv->running) {
        fd_set read_fds = srv->master_set;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };

        int activity = select(srv->max_fd + 1, &read_fds,
                              NULL, NULL, &tv);
        if (activity < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // --- 新 TCP 连接 ---
        if (FD_ISSET(srv->tcp_listen_fd, &read_fds)) {
            accept_tcp_client(srv);
        }

        // --- UDP 消息 ---
        if (FD_ISSET(srv->udp_listen_fd, &read_fds)) {
            handle_udp(srv);
        }

        // --- 已有 TCP 连接的数据 ---
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (srv->clients[i].state != CLIENT_FREE &&
                srv->clients[i].sock_fd > 0 &&
                FD_ISSET(srv->clients[i].sock_fd, &read_fds)) {
                handle_tcp_client(srv, i);
            }
        }

        // ============================================================
        //  TCP 超时检测 — ISO 13400-2
        //  select 每 5 秒超时一次，刚好用来轮询检查所有连接
        // ============================================================
        {
            time_t now = time(NULL);

            for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
                doip_client_t *cli = &srv->clients[i];
                if (cli->state == CLIENT_FREE) continue;

                // --- T_TCP_Initial_Inactivity: 连上后未路由激活的超时 ---
                if (cli->state == CLIENT_CONNECTED &&
                    now - cli->last_activity > T_TCP_INITIAL_INACTIVITY_SEC) {
                    printf("[TCP] 客户端 #%d 初始超时 (%lds 未激活)，断开\n",
                           i, (long)(now - cli->last_activity));
                    close(cli->sock_fd);
                    FD_CLR(cli->sock_fd, &srv->master_set);
                    cli->sock_fd = -1;
                    cli->state   = CLIENT_FREE;
                    continue;
                }

                // --- T_TCP_General_Inactivity: 激活后空闲超时 ---
                if (cli->state == CLIENT_ACTIVATED &&
                    now - cli->last_activity > T_TCP_GENERAL_INACTIVITY_SEC) {
                    printf("[TCP] 客户端 #%d 空闲超时 (%lds 无消息)，断开\n",
                           i, (long)(now - cli->last_activity));
                    close(cli->sock_fd);
                    FD_CLR(cli->sock_fd, &srv->master_set);
                    cli->sock_fd = -1;
                    cli->state   = CLIENT_FREE;
                    continue;
                }

                // --- T_TCP_Alive_Check: 主动发心跳保活 ---
                if (cli->state == CLIENT_ACTIVATED &&
                    now - cli->last_alive_check >= T_TCP_ALIVE_CHECK_SEC) {
                    if (send_doip_message(cli->sock_fd,
                                          DOIP_ALIVE_CHECK_REQ, NULL, 0) == 0) {
                        cli->last_alive_check = now;
                        printf("[TCP] 客户端 #%d 发送 Alive Check\n", i);
                    }
                }
            }
        }
    }
}

// ============================================================
//  停止并清理
// ============================================================
void doip_server_stop(doip_server_t *srv) {
    srv->running = 0;

    if (srv->tcp_listen_fd > 0) close(srv->tcp_listen_fd);
    if (srv->udp_listen_fd > 0) close(srv->udp_listen_fd);

    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (srv->clients[i].sock_fd > 0)
            close(srv->clients[i].sock_fd);
    }

    printf("[SERVER] 已停止\n");
}