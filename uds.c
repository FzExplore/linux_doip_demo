#include "uds.h"
#include "ota_manager.h"
#include "dtc_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "aes_cmac.h"

// 前置声明
static void download_abort(uds_context_t *ctx);

// ============================================================
//  0x27 安全访问 — AES-128 密钥（16字节）
//  生产项目应烧录到安全区域，测试用硬编码
// ============================================================
static const uint8_t SECRET_KEY[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
};

// ============================================================
//  DID 持久化文件路径
#define DID_PERSIST_PATH    "./data/did_persist.bin"   // DID 持久化文件

// ============================================================
//  模拟的 DID 数据：诊断测试仪可以读取这些值
//  dynamic=1 表示每次读取时动态生成，不从 data 字段取
// ============================================================
static struct {
    uint16_t did;
    const char *name;
    uint8_t  data[32];      // 扩大为 32 字节，容纳 VIN
    uint8_t  len;
    uint8_t  writable;      // 0=只读, 1=可写
    uint8_t  dynamic;       // 1=动态值（如 DTC 计数），读取时实时生成
} did_table[] = {
    { 0xF180, "Boot软件版本",
      {0x01, 0x00, 0x00}, 3, 0, 0 },
    { 0xF190, "VIN码",
      {'D','E','M','O','V','I','N','1','2','3','4','5','6','7','8','9','0'},
      17, 0, 0 },
    { 0xF191, "ECU硬件版本",
      {0x01, 0x00, 0x00, 0x01}, 4, 0, 0 },
    { 0xF192, "ECU软件版本",
      {0x02, 0x05, 0x00}, 3, 1, 0 },
    { 0xF193, "系统电压(V)",
      {0x00, 0x00, 0x30, 0x39}, 4, 1, 0 },  // 12.3V
    { 0xF194, "系统温度(°C)",
      {0x28}, 1, 1, 0 },                        // 40°C
    { 0x0001, "故障码数量",
      {0x00, 0x00}, 2, 0, 1 },                  // 动态生成
};

// ============================================================
//  DID 持久化：将可写 DID 保存到文件，启动时加载
//  文件格式: [did(2)][len(1)][data(len)]...
// ============================================================
static void did_persist_save(uint16_t did, const uint8_t *data, uint8_t len) {
    FILE *f = fopen(DID_PERSIST_PATH, "r+b");
    if (!f) {
        f = fopen(DID_PERSIST_PATH, "wb");
        if (!f) return;
    }

    // 检查是否已有该 DID，有则覆盖
    uint8_t buf[3];
    long pos = 0;
    int found = 0;
    while (fread(buf, 3, 1, f) == 1) {
        uint16_t exist_did = (buf[0] << 8) | buf[1];
        uint8_t  exist_len = buf[2];
        if (exist_did == did) {
            found = 1;
            break;
        }
        fseek(f, exist_len, SEEK_CUR);
        pos = ftell(f);
    }

    if (found) {
        fseek(f, pos, SEEK_SET);
    } else {
        fseek(f, 0, SEEK_END);
    }

    buf[0] = (did >> 8) & 0xFF;
    buf[1] = did & 0xFF;
    buf[2] = len;
    fwrite(buf, 3, 1, f);
    fwrite(data, len, 1, f);
    fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
    fclose(f);
}

static void did_persist_load(void) {
    FILE *f = fopen(DID_PERSIST_PATH, "rb");
    if (!f) return;  // 首次运行，无持久化文件

    uint8_t buf[32];
    while (fread(buf, 3, 1, f) == 1) {
        uint16_t did = (buf[0] << 8) | buf[1];
        uint8_t  len = buf[2];
        if (len > 32 || fread(buf, len, 1, f) != 1) break;

        // 写入内存
        for (size_t i = 0; i < sizeof(did_table) / sizeof(did_table[0]); i++) {
            if (did_table[i].did == did && did_table[i].writable) {
                memcpy(did_table[i].data, buf, len);
                did_table[i].len = len;
                printf("  [DID] 从持久化恢复 0x%04X (%s), len=%d\n",
                       did, did_table[i].name, len);
                break;
            }
        }
    }
    fclose(f);
}

// ============================================================
//  构造肯定应答：SID + 0x40 + 数据
// ============================================================
static void make_pos_response(uint8_t sid, const uint8_t *data,
                              uint8_t data_len, uint8_t *resp, uint32_t *resp_len) {
    resp[0] = UDS_SID_POS_RESP(sid);    // 肯定应答 SID
    if (data_len > 0 && data != NULL) {
        memcpy(resp + 1, data, data_len);
    }
    *resp_len = 1 + data_len;
}

// ============================================================
//  构造否定应答：0x7F + SID + NRC
// ============================================================
static void make_neg_response(uint8_t sid, uint8_t nrc,
                              uint8_t *resp, uint32_t *resp_len) {
    resp[0] = UDS_NEG_RESP_SID;    // 0x7F
    resp[1] = sid;                  // 被拒绝的服务 ID
    resp[2] = nrc;                  // 否定应答码
    *resp_len = 3;
}

// ============================================================
//  处理 0x10 诊断会话控制（不返回业务数据，只切换状态）
// ============================================================
static int handle_session_control(const uint8_t *req, uint32_t req_len,
                                  uint8_t *resp, uint32_t *resp_len,
                                  uds_context_t *ctx) {
    // 检查报文长度：SID(1) + subfunc(1)
    if (req_len < 2) {
        make_neg_response(UDS_SID_DIAG_SESSION_CONTROL,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t sub_func = req[1] & 0x7F;   // bit7 是 suppressPosRspMsg

    {
        uint8_t cur = ctx->current_session;

        // 默认 → 编程：拒绝
        if (cur == DIAG_SESSION_DEFAULT && sub_func == DIAG_SESSION_PROGRAMMING) {
            printf("  [UDS] 拒绝: 默认会话不能直接跳编程会话，请先进入扩展会话\n");
            make_neg_response(UDS_SID_DIAG_SESSION_CONTROL,
                              NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
            return 0;
        }

        // 编程 → 扩展：拒绝
        if (cur == DIAG_SESSION_PROGRAMMING && sub_func == DIAG_SESSION_EXTENDED) {
            printf("  [UDS] 拒绝: 编程会话不能直接跳扩展会话，请先退回默认\n");
            make_neg_response(UDS_SID_DIAG_SESSION_CONTROL,
                              NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
            return 0;
        }

        // 扩展 → 编程：需安全解锁
        if (cur == DIAG_SESSION_EXTENDED && sub_func == DIAG_SESSION_PROGRAMMING) {
            if (ctx->security_level < 1) {
                printf("  [UDS] 拒绝: 进入编程会话需先通过安全访问(0x27)解锁\n");
                make_neg_response(UDS_SID_DIAG_SESSION_CONTROL,
                                  NRC_SECURITY_ACCESS_DENIED, resp, resp_len);
                return 0;
            }
        }
    }

    switch (sub_func) {
    case DIAG_SESSION_DEFAULT:
        printf("  [UDS] 切换到默认会话\n");
        ctx->current_session = DIAG_SESSION_DEFAULT;
        ctx->security_level  = 0;       // 退回默认同时清安全级别
        download_abort(ctx);            // 清理未完成的下载（关文件+删半截数据）
        break;

    case DIAG_SESSION_EXTENDED:
        printf("  [UDS] 切换到扩展会话\n");
        ctx->current_session = DIAG_SESSION_EXTENDED;
        break;

    case DIAG_SESSION_PROGRAMMING:
        printf("  [UDS] 切换到编程会话\n");
        ctx->current_session = DIAG_SESSION_PROGRAMMING;
        break;

    default:
        printf("  [UDS] 不支持的子功能: 0x%02X\n", sub_func);
        make_neg_response(UDS_SID_DIAG_SESSION_CONTROL,
                          NRC_SUBFUNC_NOT_SUPPORTED, resp, resp_len);
        return 0;
    }

    // ============================================================
    //  肯定应答 — ISO 14229-2
    //  格式: 50 SUB P2_hi P2_lo P2*_hi P2*_lo
    //  P2  = 0x0032 = 50ms  (普通应答超时)
    //  P2* = 0x1388 = 5000ms (增强应答超时，如擦除Flash需更长时间)
    // ============================================================
    uint8_t pos_data[] = {
        sub_func,
        0x00, 0x32,    // P2  = 50ms
        0x13, 0x88     // P2* = 5000ms
    };
    make_pos_response(UDS_SID_DIAG_SESSION_CONTROL,
                      pos_data, sizeof(pos_data), resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x22 按 ID 读数据
// ============================================================
static int handle_read_data_by_id(const uint8_t *req, uint32_t req_len,
                                  uint8_t *resp, uint32_t *resp_len,
                                  uds_context_t *ctx) {
    (void)ctx;

    if (req_len < 3) {
        make_neg_response(UDS_SID_READ_DATA_BY_ID,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint16_t did = (req[1] << 8) | req[2];

    for (size_t i = 0; i < sizeof(did_table) / sizeof(did_table[0]); i++) {
        if (did_table[i].did == did) {
            uint8_t buf[32];
            uint8_t buf_len = did_table[i].len;

            if (did_table[i].dynamic) {
                // 动态 DID：实时生成数据
                int count = 0;
                if (did == 0x0001) {
                    count = dtc_store_count_by_status(0xFF);
                    buf[0] = (count >> 8) & 0xFF;
                    buf[1] = count & 0xFF;
                    buf_len = 2;
                }
                printf("  [UDS] 读取 DID 0x%04X (%s) = %d (动态)\n",
                       did, did_table[i].name, count);
            } else {
                memcpy(buf, did_table[i].data, did_table[i].len);
                printf("  [UDS] 读取 DID 0x%04X (%s) = ", did, did_table[i].name);
                for (int j = 0; j < did_table[i].len; j++)
                    printf("%02X ", did_table[i].data[j]);
                printf("\n");
            }

            uint8_t pos_data[64];
            pos_data[0] = (did >> 8) & 0xFF;
            pos_data[1] = did & 0xFF;
            memcpy(pos_data + 2, buf, buf_len);
            make_pos_response(UDS_SID_READ_DATA_BY_ID,
                              pos_data, 2 + buf_len, resp, resp_len);
            return 0;
        }
    }

    printf("  [UDS] DID 0x%04X 未定义\n", did);
    make_neg_response(UDS_SID_READ_DATA_BY_ID,
                      NRC_REQUEST_OUT_OF_RANGE, resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x2E 按 ID 写数据
//  请求: 2E DID_H DID_L 数据...
//  肯定应答: 6E DID_H DID_L（只回 DID，不返回数据）
//  否定应答: 7F 2E NRC（DID不存在/不可写/数据过长）
// ============================================================
static int handle_write_data_by_id(const uint8_t *req, uint32_t req_len,
                                   uint8_t *resp, uint32_t *resp_len,
                                   uds_context_t *ctx) {
    // 报文：SID(1) + DID(2) + 数据(至少0字节)
    if (req_len < 3) {
        make_neg_response(UDS_SID_WRITE_DATA_BY_ID,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint16_t did = (req[1] << 8) | req[2];
    uint32_t data_len = req_len - 3;
    const uint8_t *data = req + 3;

    // 查找 DID
    for (size_t i = 0; i < sizeof(did_table) / sizeof(did_table[0]); i++) {
        if (did_table[i].did == did) {
            // 检查是否可写
            if (!did_table[i].writable) {
                printf("  [UDS] 写 DID 0x%04X (%s) 被拒绝 (只读)\n",
                       did, did_table[i].name);
                make_neg_response(UDS_SID_WRITE_DATA_BY_ID,
                                  NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
                return 0;
            }

            // 检查数据长度
            if (data_len > 32) {
                printf("  [UDS] 写 DID 0x%04X 数据过长: %u 字节\n",
                       did, data_len);
                make_neg_response(UDS_SID_WRITE_DATA_BY_ID,
                                  NRC_REQUEST_OUT_OF_RANGE, resp, resp_len);
                return 0;
            }

            // 写入内存
            memcpy(did_table[i].data, data, data_len);
            did_table[i].len = (uint8_t)data_len;

            // 持久化到文件
            did_persist_save(did, data, data_len);

            printf("  [UDS] 写 DID 0x%04X (%s) = ", did, did_table[i].name);
            for (uint32_t j = 0; j < data_len; j++) printf("%02X ", data[j]);
            printf("\n");

            // 肯定应答：6E DID_H DID_L
            uint8_t pos_data[] = { (did >> 8) & 0xFF, did & 0xFF };
            make_pos_response(UDS_SID_WRITE_DATA_BY_ID,
                              pos_data, 2, resp, resp_len);
            return 0;
        }
    }

    printf("  [UDS] 写 DID 0x%04X 不存在\n", did);
    make_neg_response(UDS_SID_WRITE_DATA_BY_ID,
                      NRC_REQUEST_OUT_OF_RANGE, resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x31 例程控制
//  用于擦除 Flash、检查条件、CRC 校验等
// ============================================================
// 模拟 Flash 文件路径（存固件）
#define SIMULATED_FLASH_FILE OTA_STAGING_FILE

// 简单 CRC32 计算
static uint32_t calc_crc32(const uint8_t *data, size_t len, uint32_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i] << 24;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80000000)
                crc = (crc << 1) ^ 0x04C11DB7;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static int handle_routine_control(const uint8_t *req, uint32_t req_len,
                                   uint8_t *resp, uint32_t *resp_len,
                                   uds_context_t *ctx) {
    (void)ctx;

    // 报文长度：SID(1) + subfunc(1) + RoutineID(2) = 至少4字节
    if (req_len < 4) {
        make_neg_response(UDS_SID_ROUTINE_CONTROL,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t  sub_func  = req[1];
    uint16_t routine_id = (req[2] << 8) | req[3];

    printf("  [UDS] 例程控制: 0x%04X 子功能=%d\n", routine_id, sub_func);

    // --- 处理三种子功能: 启动 / 停止 / 查询结果 ---
    switch (sub_func) {
    case ROUTINE_RESULT: {
        // 查询结果：目前简化处理，返回最近一次结果
        // 真实情况需要保存结果状态
        break;
    }

    case ROUTINE_STOP: {
        // 停止例程：这里不需要，简化处理直接返回成功
        break;
    }

    case ROUTINE_START: {
        // 启动例程
        switch (routine_id) {
        // 0xFF00: 擦除 Flash → 调用 OTA 管理器创建暂存区
        case ROUTINE_ERASE_MEMORY: {
            if (ota_prepare_staging() != 0) {
                printf("  [UDS] 擦除失败: 暂存区创建失败\n");
                make_neg_response(UDS_SID_ROUTINE_CONTROL,
                                  NRC_ROUTINE_FAILED, resp, resp_len);
                return 0;
            }
            printf("  [UDS] 擦除完成: %s\n", SIMULATED_FLASH_FILE);
            break;
        }

        // 0xFF01: 检查刷写条件 → 检查目录/空间
        case ROUTINE_CHECK_CONDITION: {
            // 检查文件是否可写
            FILE *test = fopen(SIMULATED_FLASH_FILE, "rb");
            if (test) {
                fclose(test);
                printf("  [UDS] 刷写条件检查: OK\n");
            } else {
                // 文件不存在没关系，擦除会创建
                printf("  [UDS] 刷写条件检查: OK (将创建新文件)\n");
            }
            break;
        }

        // 0xFF02: CRC 校验 → 计算整个文件的 CRC32
        case ROUTINE_CHECK_CRC: {
            FILE *f = fopen(SIMULATED_FLASH_FILE, "rb");
            if (!f) {
                printf("  [UDS] CRC校验失败: 文件不存在\n");
                make_neg_response(UDS_SID_ROUTINE_CONTROL,
                                  NRC_ROUTINE_FAILED, resp, resp_len);
                return 0;
            }

            uint32_t crc = 0xFFFFFFFF;
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                crc = calc_crc32(buf, n, crc);
            }
            fclose(f);
            printf("  [UDS] CRC校验完成: CRC=0x%08X\n", crc);

            // 把 CRC 放进肯定应答: 71 + subfunc + routineID + CRC
            uint8_t pos_data[7];
            pos_data[0] = sub_func;
            pos_data[1] = (routine_id >> 8) & 0xFF;
            pos_data[2] = routine_id & 0xFF;
            pos_data[3] = (crc >> 24) & 0xFF;
            pos_data[4] = (crc >> 16) & 0xFF;
            pos_data[5] = (crc >>  8) & 0xFF;
            pos_data[6] = (crc >>  0) & 0xFF;
            make_pos_response(UDS_SID_ROUTINE_CONTROL,
                              pos_data, 7, resp, resp_len);
            return 0;
        }

        case ROUTINE_COMMIT: {
            if (ota_commit_update() != 0) {
                printf("  [UDS] Commit 失败\n");
                make_neg_response(UDS_SID_ROUTINE_CONTROL,
                                  NRC_ROUTINE_FAILED, resp, resp_len);
                return 0;
            }
            break;
        }

        case ROUTINE_ACCEPT: {
            if (ota_accept_update() != 0) {
                printf("  [UDS] Accept 失败\n");
                make_neg_response(UDS_SID_ROUTINE_CONTROL,
                                  NRC_ROUTINE_FAILED, resp, resp_len);
                return 0;
            }
            break;
        }

        default:
            printf("  [UDS] 不支持的例程: 0x%04X\n", routine_id);
            make_neg_response(UDS_SID_ROUTINE_CONTROL,
                              NRC_REQUEST_OUT_OF_RANGE, resp, resp_len);
            return 0;
        }
        break;
    }

    default:
        printf("  [UDS] 不支持的子功能: 0x%02X\n", sub_func);
        make_neg_response(UDS_SID_ROUTINE_CONTROL,
                          NRC_SUBFUNC_NOT_SUPPORTED, resp, resp_len);
        return 0;
    }

    // 肯定应答: 71 subfunc + RoutineID(2字节)
    uint8_t pos_data[3];
    pos_data[0] = sub_func;
    pos_data[1] = (routine_id >> 8) & 0xFF;
    pos_data[2] = routine_id & 0xFF;
    make_pos_response(UDS_SID_ROUTINE_CONTROL,
                      pos_data, 3, resp, resp_len);
    return 0;
}

// ============================================================
//  下载中断清理：关闭文件、删除半截数据、重置下载状态
//  在以下情况调用：退回默认会话、连接断开、下载出错
// ============================================================
static void download_abort(uds_context_t *ctx) {
    if (!ctx->download_active) return;

    if (ctx->download_file) {
        fclose(ctx->download_file);
        ctx->download_file = NULL;
    }

    // 删除半截的 staging 文件，防止残留脏数据
    remove(SIMULATED_FLASH_FILE);

    ctx->download_active    = 0;
    ctx->download_written   = 0;
    ctx->download_size      = 0;
    ctx->download_block_seq = 0;

    printf("  [UDS] 下载已中断，暂存区已清理\n");
}

// ============================================================
//  处理 0x34 请求下载
//  请求: 34 00 44 addr(4) size(4)
//  肯定: 74 00 44 maxBlockSize(4)
// ============================================================
#define MAX_BLOCK_SIZE  4096    // 每个 TransferData 块最大 4KB

static int handle_request_download(const uint8_t *req, uint32_t req_len,
                                    uint8_t *resp, uint32_t *resp_len,
                                    uds_context_t *ctx) {
    // 报文: SID(1) + dataFormat(1) + addrLenFmt(1) + addr(4) + size(4)
    if (req_len < 11) {
        make_neg_response(UDS_SID_REQUEST_DOWNLOAD,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t  data_format = req[1];
    uint8_t  addr_lenfmt = req[2];
    uint32_t addr = (req[3] << 24) | (req[4] << 16) | (req[5] << 8) | req[6];
    uint32_t size = (req[7] << 24) | (req[8] << 16) | (req[9] << 8) | req[10];

    printf("  [UDS] 请求下载: 格式=0x%02X/0x%02X 地址=0x%08X 大小=%u字节\n",
           data_format, addr_lenfmt, addr, size);

    // 如果上次下载异常中断，先清理残留
    download_abort(ctx);

    // 初始化下载状态
    ctx->download_active    = 1;
    ctx->download_addr      = addr;
    ctx->download_size      = size;
    ctx->download_written   = 0;
    ctx->download_block_seq = 1;

    // 打开文件（全程保持，36 复用，37 关闭）
    ctx->download_file = fopen(SIMULATED_FLASH_FILE, "wb");
    if (!ctx->download_file) {
        printf("  [UDS] 请求下载-无法创建文件\n");
        make_neg_response(UDS_SID_REQUEST_DOWNLOAD,
                          NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
        ctx->download_active = 0;

    // 一次性刷盘 + 关闭文件（协议保证：37 是检查点）
    if (ctx->download_file) {
        fflush(ctx->download_file);
        fsync(fileno(ctx->download_file));
        fclose(ctx->download_file);
        ctx->download_file = NULL;
    }

    printf("  [UDS] 传输退出-数据已落盘\n");
        return 0;
    }

    // 肯定应答: 74 00 44 + maxBlockSize(4字节)
    uint8_t pos_data[6];
    pos_data[0] = 0x00;  // lengthFormatIdentifier
    pos_data[1] = 0x44;  // 4字节地址 + 4字节长度
    pos_data[2] = (MAX_BLOCK_SIZE >> 24) & 0xFF;
    pos_data[3] = (MAX_BLOCK_SIZE >> 16) & 0xFF;
    pos_data[4] = (MAX_BLOCK_SIZE >>  8) & 0xFF;
    pos_data[5] = (MAX_BLOCK_SIZE >>  0) & 0xFF;
    make_pos_response(UDS_SID_REQUEST_DOWNLOAD,
                      pos_data, 6, resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x36 传输数据
//  请求: 36 blockSeq(1) data...
//  肯定: 76 blockSeq(1)
// ============================================================
static int handle_transfer_data(const uint8_t *req, uint32_t req_len,
                                 uint8_t *resp, uint32_t *resp_len,
                                 uds_context_t *ctx) {
    if (!ctx->download_active) {
        make_neg_response(UDS_SID_TRANSFER_DATA,
                          NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
        return 0;
    }

    // 报文: SID(1) + blockSeq(1) + data(至少1字节)
    if (req_len < 3) {
        make_neg_response(UDS_SID_TRANSFER_DATA,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t block_seq = req[1];
    uint32_t data_len = req_len - 2;
    const uint8_t *data = req + 2;

    // 检查块序号
    if (block_seq != ctx->download_block_seq) {
        printf("  [UDS] 传输数据-块序号错误: 期望=%d 收到=%d\n",
               ctx->download_block_seq, block_seq);
        make_neg_response(UDS_SID_TRANSFER_DATA,
                          NRC_REQUEST_OUT_OF_RANGE, resp, resp_len);
        return 0;
    }

    // 写入模拟 Flash 文件（复用 34 打开的句柄，不逐块开闭）
    fseek(ctx->download_file, (long)ctx->download_written, SEEK_SET);
    fwrite(data, 1, data_len, ctx->download_file);

    ctx->download_written += data_len;
    ctx->download_block_seq++;

    printf("  [UDS] 传输数据-块#%d: %u字节 进度=%u/%u\n",
           block_seq, data_len, ctx->download_written, ctx->download_size);

    // 肯定应答: 76 + blockSeq
    uint8_t pos_data[] = { block_seq };
    make_pos_response(UDS_SID_TRANSFER_DATA,
                      pos_data, 1, resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x37 传输退出
//  请求: 37
//  肯定: 77
// ============================================================
static int handle_transfer_exit(const uint8_t *req, uint32_t req_len,
                                 uint8_t *resp, uint32_t *resp_len,
                                 uds_context_t *ctx) {
    (void)req;

    if (!ctx->download_active) {
        make_neg_response(UDS_SID_TRANSFER_EXIT,
                          NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
        return 0;
    }

    if (req_len < 1) {
        make_neg_response(UDS_SID_TRANSFER_EXIT,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    printf("  [UDS] 传输退出: 已写%u字节 / 总%u字节\n",
           ctx->download_written, ctx->download_size);

    // 检查是否写完了
    if (ctx->download_written != ctx->download_size) {
        printf("  [UDS] 传输退出-大小不匹配!\n");
        make_neg_response(UDS_SID_TRANSFER_EXIT,
                          NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
        ctx->download_active = 0;
        return 0;
    }

    ctx->download_active = 0;

    // 肯定应答: 77
    uint8_t pos_data[] = { 0x00 };  // 无参数，但 ISO 要求至少1字节
    make_pos_response(UDS_SID_TRANSFER_EXIT,
                      pos_data, 0, resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x3E Tester Present
// ============================================================
static int handle_tester_present(const uint8_t *req, uint32_t req_len,
                                 uint8_t *resp, uint32_t *resp_len,
                                 uds_context_t *ctx) {
    if (req_len < 2) {
        make_neg_response(UDS_SID_TESTER_PRESENT,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t sub_func = req[1];
    ctx->tester_present_count++;
    printf("  [UDS] Tester Present (第%d次) sub=0x%02X\n",
           ctx->tester_present_count, sub_func);

    // 零子功能不回复，子功能 0x00=不回复, 0x01=回复
    if (sub_func == 0x00) {
        *resp_len = 0;  // 不回复
        return 0;
    }

    // 肯定应答 = SID|0x40 + subfunc
    uint8_t pos_data[] = { sub_func };
    make_pos_response(UDS_SID_TESTER_PRESENT,
                      pos_data, sizeof(pos_data), resp, resp_len);
    return 0;
}

// ============================================================
//  处理 0x11 ECU 复位
// ============================================================
static int handle_ecu_reset(const uint8_t *req, uint32_t req_len,
                            uint8_t *resp, uint32_t *resp_len,
                            uds_context_t *ctx) {
    if (req_len < 2) {
        make_neg_response(UDS_SID_ECU_RESET,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t reset_type = req[1] & 0x7F;
    printf("  [UDS] ECU 复位请求: type=0x%02X\n", reset_type);

    switch (reset_type) {
    case 0x01:  // 硬复位 — 重启整个进程
        printf("  [UDS] 执行硬复位 — 进程将重启\n");
        ctx->reset_pending = 1;
        break;

    case 0x03:  // 软复位 — 重置 UDS 状态
        printf("  [UDS] 执行软复位 — 重置会话/安全级别\n");
        ctx->current_session  = DIAG_SESSION_DEFAULT;
        ctx->security_level   = 0;
        download_abort(ctx);  // 清理未完成的下载（关文件+删半截数据）
        ctx->reset_pending    = 0;
        break;

    default:
        printf("  [UDS] 不支持的复位类型: 0x%02X\n", reset_type);
        make_neg_response(UDS_SID_ECU_RESET,
                          NRC_SUBFUNC_NOT_SUPPORTED, resp, resp_len);
        return 0;
    }

    uint8_t pos_data[] = { reset_type };
    make_pos_response(UDS_SID_ECU_RESET,
                      pos_data, sizeof(pos_data), resp, resp_len);
    return 0;
}

// ============================================================
//  0x14 清除 DTC — ISO 14229-1
//  请求格式: 14 FF FF FF  → 清除所有 DTC
//            14 xx xx xx  → 清除指定 DTC 组
//  会话要求: 扩展会话或编程会话
// ============================================================
static int handle_clear_dtc(const uint8_t *req, uint32_t req_len,
                            uint8_t *resp, uint32_t *resp_len,
                            uds_context_t *ctx) {
    if (req_len < 4) {
        make_neg_response(UDS_SID_CLEAR_DTC,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    // 清除 DTC 需要扩展会话或编程会话
    if (ctx->current_session != DIAG_SESSION_EXTENDED &&
        ctx->current_session != DIAG_SESSION_PROGRAMMING) {
        make_neg_response(UDS_SID_CLEAR_DTC,
                          NRC_CONDITIONS_NOT_CORRECT, resp, resp_len);
        return 0;
    }

    uint32_t dtc_group = (req[1] << 16) | (req[2] << 8) | req[3];
    printf("  [UDS] 清除 DTC: group=0x%06X\n", dtc_group);

    if (dtc_group == CLEAR_DTC_GROUP_OF_DTC) {
        dtc_store_clear_all();
        printf("  [UDS] 已清除所有 DTC\n");
    } else {
        dtc_store_clear_group(dtc_group);
        printf("  [UDS] 已清除 DTC 组 0x%02X\n", dtc_group & 0xFF);
    }

    // 肯定应答：54
    uint8_t pos_data = 0x00;
    make_pos_response(UDS_SID_CLEAR_DTC,
                      &pos_data, 1, resp, resp_len);
    return 0;
}

// ============================================================
//  0x19 读取 DTC — ISO 14229-1
//  子功能:
//  0x01: 按状态掩码读取 DTC 数量
//  0x02: 按状态掩码读取 DTC 列表
//  0x0A: 读取所有支持的 DTC
// ============================================================
static int handle_read_dtc(const uint8_t *req, uint32_t req_len,
                           uint8_t *resp, uint32_t *resp_len,
                           uds_context_t *ctx) {
    if (req_len < 2) {
        make_neg_response(UDS_SID_READ_DTC,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t sub_func = req[1];

    switch (sub_func) {
    // ----------------------------------------------------------
    //  0x19 0x01: 按状态掩码读取 DTC 数量
    //  请求: 19 01 xx
    //  响应: 59 01 status_mask dtc_format(1) count(2)
    // ----------------------------------------------------------
    case READ_DTC_COUNT_BY_STATUS_MASK: {
        if (req_len < 3) {
            make_neg_response(UDS_SID_READ_DTC,
                              NRC_INCORRECT_LENGTH, resp, resp_len);
            return 0;
        }

        uint8_t status_mask = req[2];
        int count = dtc_store_count_by_status(status_mask);

        printf("  [UDS] 读 DTC 数量: mask=0x%02X, count=%d\n",
               status_mask, count);

        uint8_t pos_data[4];
        pos_data[0] = sub_func;
        pos_data[1] = status_mask;
        pos_data[2] = 0x01;  // DTC 格式: ISO 14229-1
        pos_data[3] = (uint8_t)count;
        make_pos_response(UDS_SID_READ_DTC,
                          pos_data, 4, resp, resp_len);
        break;
    }

    // ----------------------------------------------------------
    //  0x19 0x02: 按状态掩码读取 DTC 列表
    //  请求: 19 02 xx
    //  响应: 59 02 status_mask dtc_format(1) dtc_1(4) dtc_1_status(1) ...
    // ----------------------------------------------------------
    case READ_DTC_BY_STATUS_MASK: {
        if (req_len < 3) {
            make_neg_response(UDS_SID_READ_DTC,
                              NRC_INCORRECT_LENGTH, resp, resp_len);
            return 0;
        }

        uint8_t status_mask = req[2];
        dtc_record_t records[DTC_MAX_COUNT];
        int count = dtc_store_read_by_status(status_mask, records,
                                             DTC_MAX_COUNT);

        printf("  [UDS] 读 DTC 列表: mask=0x%02X, count=%d\n",
               status_mask, count);

        // 构造响应: 59 02 status_mask format(1) [dtc(3)+status(1)]*n
        uint8_t pos_data[256];
        int pos = 0;
        pos_data[pos++] = sub_func;
        pos_data[pos++] = status_mask;
        pos_data[pos++] = 0x01;  // DTC 格式
        for (int i = 0; i < count; i++) {
            // DTC 码 3 字节
            pos_data[pos++] = (records[i].dtc_code >> 16) & 0xFF;
            pos_data[pos++] = (records[i].dtc_code >> 8)  & 0xFF;
            pos_data[pos++] = (records[i].dtc_code)       & 0xFF;
            // 状态
            pos_data[pos++] = records[i].status_mask;
        }
        make_pos_response(UDS_SID_READ_DTC,
                          pos_data, pos, resp, resp_len);
        break;
    }

    // ----------------------------------------------------------
    //  0x19 0x04: 按 DTC 码读取快照记录
    //  请求: 19 04 dtc_h dtc_m dtc_l record_num
    //  响应: 59 04 dtc_h dtc_m dtc_l status record_num [快照数据]
    // ----------------------------------------------------------
    case READ_DTC_SNAPSHOT_BY_DTC: {
        if (req_len < 6) {
            make_neg_response(UDS_SID_READ_DTC,
                              NRC_INCORRECT_LENGTH, resp, resp_len);
            return 0;
        }

        uint32_t dtc_code = (req[2] << 16) | (req[3] << 8) | req[4];
        uint8_t  rec_num  = req[5];

        printf("  [UDS] 读快照: DTC=0x%06X, record=%d\n", dtc_code, rec_num);

        dtc_snapshot_t snap;
        int found = dtc_store_snapshot_read(dtc_code, rec_num, &snap);

        if (!found) {
            make_neg_response(UDS_SID_READ_DTC,
                              NRC_REQUEST_OUT_OF_RANGE, resp, resp_len);
            return 0;
        }

        // 构造响应: 59 04 dtc(3) status(1) rec_num(1) [快照数据(12)]
        uint8_t pos_data[32];
        int pos = 0;
        pos_data[pos++] = sub_func;
        pos_data[pos++] = (dtc_code >> 16) & 0xFF;
        pos_data[pos++] = (dtc_code >> 8)  & 0xFF;
        pos_data[pos++] = (dtc_code)       & 0xFF;

        // 查 DTC 当前状态
        pos_data[pos++] = 0x08;  // 简化：假定已确认

        pos_data[pos++] = rec_num;

        // 快照数据：电池电压(2) + 温度(2) + 里程(2)
        pos_data[pos++] = (snap.battery_voltage >> 8) & 0xFF;
        pos_data[pos++] = (snap.battery_voltage)      & 0xFF;
        pos_data[pos++] = (snap.temperature >> 8) & 0xFF;
        pos_data[pos++] = (snap.temperature)      & 0xFF;
        pos_data[pos++] = (snap.mileage >> 8) & 0xFF;
        pos_data[pos++] = (snap.mileage)      & 0xFF;

        make_pos_response(UDS_SID_READ_DTC,
                          pos_data, pos, resp, resp_len);
        break;
    }

    // ----------------------------------------------------------
    //  0x19 0x0A: 读取 ECU 支持的 DTC 列表（预定义，非当前发生的）
    //  响应: 59 0A status_mask dtc_format(1) [dtc(3)+status(1)]*n
    // ----------------------------------------------------------
    case READ_DTC_SUPPORTED: {
        dtc_record_t records[DTC_MAX_COUNT];
        int count = dtc_store_get_supported(records, DTC_MAX_COUNT);

        printf("  [UDS] 读支持的 DTC 列表: count=%d\n", count);

        uint8_t pos_data[512];
        int pos = 0;
        pos_data[pos++] = sub_func;
        pos_data[pos++] = 0xFF;  // 返回所有状态
        pos_data[pos++] = 0x01;  // DTC 格式
        for (int i = 0; i < count; i++) {
            pos_data[pos++] = (records[i].dtc_code >> 16) & 0xFF;
            pos_data[pos++] = (records[i].dtc_code >> 8)  & 0xFF;
            pos_data[pos++] = (records[i].dtc_code)       & 0xFF;
            pos_data[pos++] = records[i].status_mask;
        }
        make_pos_response(UDS_SID_READ_DTC,
                          pos_data, pos, resp, resp_len);
        break;
    }

    default:
        printf("  [UDS] 0x19 不支持的子功能: 0x%02X\n", sub_func);
        make_neg_response(UDS_SID_READ_DTC,
                          NRC_SUBFUNC_NOT_SUPPORTED, resp, resp_len);
        break;
    }

    return 0;
}

// ============================================================
//  处理 0x27 安全访问（挑战-应答两步骤）
//  步骤1: 27 01 → 生成随机种子，返回 67 01 + 种子(4字节)
//  步骤2: 27 02 + 密钥(4字节) → 验证密钥，对则解锁，错则拒绝
//  密钥算法：key = ~seed（种子按位取反，最简单）
// ============================================================
static int handle_security_access(const uint8_t *req, uint32_t req_len,
                                  uint8_t *resp, uint32_t *resp_len,
                                  uds_context_t *ctx) {
    if (req_len < 2) {
        make_neg_response(UDS_SID_SECURITY_ACCESS,
                          NRC_INCORRECT_LENGTH, resp, resp_len);
        return 0;
    }

    uint8_t sub_func = req[1];

    switch (sub_func) {
    // ----------------------------------------------------------
    //  步骤1：请求种子 (27 01)
    // ----------------------------------------------------------
    case SECURITY_ACCESS_REQ_SEED: {
        // 生成 4 字节随机种子
        srand(time(NULL));
        for (int i = 0; i < 4; i++) {
            ctx->seed[i] = rand() & 0xFF;
        }

        printf("  [UDS] 安全访问-请求种子, 种子=");
        for (int i = 0; i < 4; i++) printf("%02X ", ctx->seed[i]);
        printf("\n");

        // 肯定应答：67 01 + 种子
        uint8_t pos_data[5];
        pos_data[0] = SECURITY_ACCESS_REQ_SEED;
        memcpy(pos_data + 1, ctx->seed, 4);
        make_pos_response(UDS_SID_SECURITY_ACCESS,
                          pos_data, 5, resp, resp_len);
        break;
    }

    // ----------------------------------------------------------
    //  步骤2：发送密钥 (27 02)
    // ----------------------------------------------------------
    case SECURITY_ACCESS_SEND_KEY: {
        if (req_len < 6) {
            make_neg_response(UDS_SID_SECURITY_ACCESS,
                              NRC_INCORRECT_LENGTH, resp, resp_len);
            return 0;
        }

        // 取出测试仪发来的密钥（4字节）
        const uint8_t *key = req + 2;

        // 计算预期密钥：AES-CMAC(密钥, 种子)，取前4字节
        uint8_t expected_cmac[16];
        aes_cmac(SECRET_KEY, ctx->seed, 4, expected_cmac);

        uint8_t expected_key[4];
        memcpy(expected_key, expected_cmac, 4);

        printf("  [UDS] 安全访问-收到密钥: ");
        for (int i = 0; i < 4; i++) printf("%02X ", key[i]);
        printf("  预期密钥(AES-CMAC): ");
        for (int i = 0; i < 4; i++) printf("%02X ", expected_key[i]);
        printf("\n");

        // 比较密钥
        if (memcmp(key, expected_key, 4) == 0) {
            ctx->security_level = 1;
            printf("  [UDS] 安全访问-解锁成功! security_level=%d\n",
                   ctx->security_level);

            // 肯定应答：67 02
            uint8_t pos_data[] = { SECURITY_ACCESS_SEND_KEY };
            make_pos_response(UDS_SID_SECURITY_ACCESS,
                              pos_data, 1, resp, resp_len);
        } else {
            printf("  [UDS] 安全访问-密钥错误，拒绝!\n");
            make_neg_response(UDS_SID_SECURITY_ACCESS,
                              NRC_INVALID_KEY, resp, resp_len);
        }
        break;
    }

    default:
        printf("  [UDS] 安全访问-不支持的子功能: 0x%02X\n", sub_func);
        make_neg_response(UDS_SID_SECURITY_ACCESS,
                          NRC_SUBFUNC_NOT_SUPPORTED, resp, resp_len);
        break;
    }

    return 0;
}

// ============================================================
//  UDS 模块初始化 — 启动时从文件恢复可写 DID 的值
// ============================================================
void uds_init(void) {
    did_persist_load();
}

// ============================================================
//  入口函数：收到 UDS 请求 → 分发给各服务
// ============================================================
//               请求: 10 01               响应: 50 04 00 32 00 C8
//               SID subfunc              SID|0x40 subfunc P2 P2*
//
//       否定应答结构:
//               请求: 10 FF               响应: 7F 10 12
//               SID subfunc(不支持)       0x7F SID NRC
// ============================================================
int uds_process_message(const uint8_t *req, uint32_t req_len,
                        uint8_t *resp, uint32_t *resp_len,
                        uds_context_t *ctx) {
    if (req_len < 1) return -1;

    uint8_t sid = req[0];

    // ============================================================
    //  S3 会话超时检查 — ISO 14229-2
    //  非默认会话（扩展/编程）超过 S3_TIMEOUT_SEC 没收到请求，
    //  自动退回默认会话并清除安全级别
    // ============================================================
    if (ctx->current_session != DIAG_SESSION_DEFAULT) {
        time_t now = time(NULL);
        if (now - ctx->last_activity > S3_TIMEOUT_SEC) {
            printf("  [UDS] S3 会话超时 (%ld 秒无请求)，退回默认会话\n",
                   (long)(now - ctx->last_activity));
            ctx->current_session  = DIAG_SESSION_DEFAULT;
            ctx->security_level   = 0;
            download_abort(ctx);  // 清理未完成的下载（关文件+删半截数据）
        }
    }

    // 更新最后活动时间（3E TesterPresent 也算活动，后面单独判断再更新也不影响）
    ctx->last_activity = time(NULL);

    printf("  [UDS] 收到服务: 0x%02X\n", sid);

    switch (sid) {
    case UDS_SID_DIAG_SESSION_CONTROL:
        return handle_session_control(req, req_len, resp, resp_len, ctx);

    case UDS_SID_READ_DATA_BY_ID:
        return handle_read_data_by_id(req, req_len, resp, resp_len, ctx);

    case UDS_SID_TESTER_PRESENT:
        return handle_tester_present(req, req_len, resp, resp_len, ctx);

    case UDS_SID_ECU_RESET:
        return handle_ecu_reset(req, req_len, resp, resp_len, ctx);

    case UDS_SID_CLEAR_DTC:
        return handle_clear_dtc(req, req_len, resp, resp_len, ctx);

    case UDS_SID_READ_DTC:
        return handle_read_dtc(req, req_len, resp, resp_len, ctx);

    case UDS_SID_SECURITY_ACCESS:
        return handle_security_access(req, req_len, resp, resp_len, ctx);

    case UDS_SID_WRITE_DATA_BY_ID:
        return handle_write_data_by_id(req, req_len, resp, resp_len, ctx);

    case UDS_SID_ROUTINE_CONTROL:
        return handle_routine_control(req, req_len, resp, resp_len, ctx);

    case UDS_SID_REQUEST_DOWNLOAD:
        return handle_request_download(req, req_len, resp, resp_len, ctx);

    case UDS_SID_TRANSFER_DATA:
        return handle_transfer_data(req, req_len, resp, resp_len, ctx);

    case UDS_SID_TRANSFER_EXIT:
        return handle_transfer_exit(req, req_len, resp, resp_len, ctx);

    default:
        printf("  [UDS] 不支持的服务: 0x%02X\n", sid);
        make_neg_response(sid, NRC_SERVICE_NOT_SUPPORTED,
                          resp, resp_len);
        return 0;
    }
}