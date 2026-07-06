/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-11 18:04:33
****LastEditors: Do not edit
****LastEditTime: 2026-06-17 18:47:30
****Description: 
****FilePath: \demo\uds.h
********************************************************************************/
#ifndef UDS_H
#define UDS_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

// ============================================================
//  清除 DTC（0x14）的子功能
// ============================================================
#define CLEAR_DTC_GROUP_OF_DTC     0xFFFFFF  // 清除所有 DTC（groupOfDTC=FFFFFF）

// ============================================================
//  读取 DTC（0x19）的子功能
// ============================================================
#define READ_DTC_BY_STATUS_MASK        0x02   // 按状态掩码读取 DTC 列表
#define READ_DTC_COUNT_BY_STATUS_MASK  0x01   // 按状态掩码读取 DTC 数量
#define READ_DTC_SNAPSHOT_BY_DTC       0x04   // 按 DTC 码读取快照
#define READ_DTC_SUPPORTED             0x0A   // 读取支持的 DTC

// ============================================================
//  UDS 服务 ID（ISO 14229-1）
// ============================================================
#define UDS_SID_DIAG_SESSION_CONTROL    0x10    // 诊断会话控制
#define UDS_SID_ECU_RESET               0x11    // ECU 复位
#define UDS_SID_CLEAR_DTC               0x14    // 清除故障码
#define UDS_SID_READ_DTC                0x19    // 读 DTC 信息
#define UDS_SID_READ_DATA_BY_ID         0x22    // 按 ID 读数据
#define UDS_SID_SECURITY_ACCESS         0x27    // 安全访问
#define UDS_SID_ROUTINE_CONTROL         0x31    // 例程控制
#define UDS_SID_REQUEST_DOWNLOAD        0x34    // 请求下载
#define UDS_SID_TRANSFER_DATA           0x36    // 传输数据
#define UDS_SID_TRANSFER_EXIT           0x37    // 传输退出
#define UDS_SID_WRITE_DATA_BY_ID        0x2E    // 按 ID 写数据
#define UDS_SID_TESTER_PRESENT          0x3E    // Tester Present

// ============================================================
//  UDS 响应码（肯定/否定）
// ============================================================
#define UDS_POS_RESP_MASK               0x40    // 肯定应答：SID | 0x40
// 例如：请求 0x10 → 肯定应答 0x50
#define UDS_SID_POS_RESP(sid)           ((sid) | UDS_POS_RESP_MASK)

#define UDS_NEG_RESP_SID                0x7F    // 否定应答

// 否定应答码（NRC）
#define NRC_GENERAL_REJECT              0x10    // 通用拒绝
#define NRC_SERVICE_NOT_SUPPORTED       0x11    // 服务不支持
#define NRC_SUBFUNC_NOT_SUPPORTED       0x12    // 子功能不支持
#define NRC_INCORRECT_LENGTH            0x13    // 报文长度错误
#define NRC_CONDITIONS_NOT_CORRECT      0x22    // 条件不满足
#define NRC_REQUEST_OUT_OF_RANGE        0x31    // 请求超出范围
#define NRC_SECURITY_ACCESS_DENIED      0x33    // 安全访问拒绝
#define NRC_INVALID_KEY                 0x35    // 密钥无效
#define NRC_ROUTINE_FAILED              0x72    // 例程执行失败

// ============================================================
//  例程控制（0x31）的子功能
// ============================================================
#define ROUTINE_START                   0x01    // 启动例程
#define ROUTINE_STOP                    0x02    // 停止例程
#define ROUTINE_RESULT                  0x03    // 查询结果

// 刷写相关例程 ID
#define ROUTINE_ERASE_MEMORY            0xFF00  // 擦除 Flash
#define ROUTINE_CHECK_CONDITION         0xFF01  // 检查刷写条件
#define ROUTINE_CHECK_CRC               0xFF02  // CRC 校验
#define ROUTINE_COMMIT                  0xFF03  // 提交更新（暂存区→非活跃槽）
#define ROUTINE_ACCEPT                  0xFF04  // 确认更新（TESTING→OK，部署到供应商目录）

// ============================================================
//  安全访问（0x27）的子功能
// ============================================================
#define SECURITY_ACCESS_REQ_SEED        0x01    // 请求种子
#define SECURITY_ACCESS_SEND_KEY        0x02    // 发送密钥

// ============================================================
//  诊断会话类型（0x10 的子功能）
// ============================================================
#define DIAG_SESSION_DEFAULT            0x01    // 默认会话
#define DIAG_SESSION_PROGRAMMING        0x02    // 编程会话
#define DIAG_SESSION_EXTENDED           0x03    // 扩展会话

// ============================================================
//  会话超时时间参数 — ISO 14229-2
// ============================================================
#define S3_TIMEOUT_SEC  5     // S3: 非默认会话超时 5 秒，到期退回默认会话
typedef struct {
    uint8_t  current_session;       // 当前诊断会话类型
    uint8_t  security_level;        // 安全级别（0=未解锁，1=已解锁）
    uint8_t  tester_present_count;  // Tester Present 计数器
    uint8_t  seed[4];               // 本次生成的种子（验证密钥时比对）

    // 会话时间管理
    time_t   last_activity;           // 上次收到 UDS 请求的时间，用于 S3 超时检测

    // 下载状态（0x34/0x36/0x37 使用）
    uint8_t  download_active;       // 是否正在下载中（0=否, 1=是）
    uint32_t download_addr;         // 目标地址
    uint32_t download_size;         // 总大小
    uint32_t download_written;      // 已写入字节数
    uint8_t  download_block_seq;    // 期望的下一个块序号
    FILE    *download_file;         // 下载文件句柄（34打开，36写入，37关闭）
    uint8_t  reset_pending;        // 0=正常, 1=需要硬复位, 2=需要软复位
} uds_context_t;

// ============================================================
//  UDS 消息处理：请求进来 → 响应出去
//  req: 请求数据指针, req_len: 请求长度
//  resp: 响应数据缓冲区, resp_len: 响应实际长度（出参）
//  ctx: UDS 上下文
//  返回值：0 成功，-1 失败
// ============================================================
int uds_process_message(const uint8_t *req, uint32_t req_len,
                        uint8_t *resp, uint32_t *resp_len,
                        uds_context_t *ctx);

void uds_init(void);

#endif // UDS_H