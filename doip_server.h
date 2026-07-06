/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-11 16:56:02
****LastEditors: Do not edit
****LastEditTime: 2026-06-16 16:11:48
****Description: 
****FilePath: \demo\doip_server.h
********************************************************************************/
#ifndef DOIP_SERVER_H
#define DOIP_SERVER_H

#include "doip.h"
#include <stdint.h>
#include <sys/select.h>
#include <time.h>

// ============================================================
//  服务器配置
// ============================================================
#define MAX_TCP_CLIENTS         5       // 最大并发连接数
#define RECV_BUFFER_SIZE        8192    // 接收缓冲区大小（需容纳 4KB数据+DoIP头）

// ============================================================
//  DoIP 时间参数 — ISO 13400-2
// ============================================================
#define T_TCP_INITIAL_INACTIVITY_SEC  5    // TCP 连上后等路由激活的超时
#define T_TCP_GENERAL_INACTIVITY_SEC  30   // TCP 空闲超时，到期断开连接
#define T_TCP_ALIVE_CHECK_SEC         2    // ECU 主动发 0x0007 心跳的间隔
#define A_DOIP_CTRL_SEC               5    // 等待控制消息响应的超时（诊断仪侧）
#define A_PROCESS_TIME_SEC            5    // 处理诊断消息的最长时间（ECU 侧）

// ============================================================
//  TCP 客户端状态 — 一个连接就是一个诊断会话
// ============================================================
enum client_state {
    CLIENT_FREE = 0,            // 槽位空闲
    CLIENT_CONNECTED,           // TCP 已建立连接
    CLIENT_REGISTERED,          // 已收到车辆识别请求
    CLIENT_ACTIVATED,           // 路由已激活，可收发诊断数据
};

// ============================================================
//  单个 TCP 客户端信息
// ============================================================
typedef struct {
    int           sock_fd;              // socket 文件描述符
    enum client_state state;            // 当前状态
    uint16_t      tester_addr;          // 测试仪逻辑地址（路由激活后分配）
    uint8_t       recv_buf[RECV_BUFFER_SIZE];  // 接收缓冲区
    uint32_t      recv_len;             // 已接收字节数

    time_t        last_activity;        // 上次收到 TCP 消息的时间（超时检测用）
    time_t        last_alive_check;     // 上次发送 Alive Check 的时间
} doip_client_t;

// ============================================================
//  DoIP 服务器上下文 — 管理所有 socket 和客户端
// ============================================================
typedef struct {
    int             tcp_listen_fd;      // TCP 监听 socket
    int             udp_listen_fd;      // UDP 监听 socket
    int             max_fd;             // select 用的最大 fd
    fd_set          master_set;         // select 主集合

    doip_client_t   clients[MAX_TCP_CLIENTS];  // TCP 客户端数组

    // 模拟车辆信息
    uint8_t         vin[17];            // VIN 码
    uint16_t        logical_address;    // 本 ECU 逻辑地址
    uint8_t         eid[6];             // EID
    uint8_t         gid[6];             // GID

    volatile int    running;            // 运行标志
} doip_server_t;

// ============================================================
//  API 函数声明
// ============================================================
int  doip_server_init(doip_server_t *srv);
int  doip_server_start(doip_server_t *srv);
void doip_server_stop(doip_server_t *srv);
void doip_server_run(doip_server_t *srv);

#endif // DOIP_SERVER_H