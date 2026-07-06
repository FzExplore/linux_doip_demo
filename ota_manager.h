/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-12 16:00:00
****Description: 工业标准 AB 分区 OTA 管理器
****    - 暂存区接收固件 → 校验 → Commit 到非活跃槽 → 重启验证
****    - 元数据持久化在 /ota/ota_status，含 CRC8 校验
****    - 槽位状态机: INACTIVE → TESTING → OK / FAILED
****FilePath: \demo\ota_manager.h
********************************************************************************/
#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdint.h>

// ============================================================
//  目录与文件路径定义 — 与 bootloader 共享的接口
// ============================================================
#define OTA_BASE_DIR        "./data/ota"                   // OTA 根目录
#define OTA_STAGING_FILE    OTA_BASE_DIR "/staging/firmware.bin"   // 暂存区
#define OTA_SLOT_A_FILE     OTA_BASE_DIR "/slot_a/firmware.bin"    // A 槽
#define OTA_SLOT_B_FILE     OTA_BASE_DIR "/slot_b/firmware.bin"    // B 槽
#define OTA_STATUS_FILE     OTA_BASE_DIR "/ota_status"             // 元数据
#define VENDOR_UPDATE_PATH  "./data/ota/vendor/firmware.bin"  // 供应商目录

// ============================================================
//  槽位状态枚举 — 与 bootloader 共享的状态机定义
// ============================================================
#define SLOT_STATUS_INACTIVE    0   // 非活跃：从未被写入过
#define SLOT_STATUS_OK          1   // 正常：已验证通过，可安全运行
#define SLOT_STATUS_TESTING     2   // 待验证：刚 Commit，运行时需确认
#define SLOT_STATUS_FAILED      3   // 失败：尝试次数耗尽，不可用

#define MAX_BOOT_TRIES          3   // 新固件最多启动尝试次数

// ============================================================
//  元数据结构 — 写入 /ota/ota_status 文件
//  与 bootloader 共享，任何字段改动需同步更新！
// ============================================================
typedef struct {
    uint8_t  magic[4];          // 魔数 "OTA!"，校验文件有效性
    uint8_t  active_slot;       // 当前活跃槽: 'a' 或 'b'
    uint8_t  slot_a_status;     // A 槽状态（见 SLOT_STATUS_xxx）
    uint8_t  slot_b_status;     // B 槽状态
    uint8_t  slot_a_tries;      // A 槽剩余启动尝试次数
    uint8_t  slot_b_tries;      // B 槽剩余启动尝试次数
    uint8_t  reserved[6];       // 保留字段（未来扩展用，当前填 0）
    uint8_t  crc8;              // 前 16 字节的 CRC8（多项式 0x1D），防止数据损坏
} ota_status_t;

// ============================================================
//  接口函数 — 供 UDS 层调用
// ============================================================

// 初始化 OTA 子系统（创建目录、读取/写入元数据）
// 返回: 0 成功，-1 失败
int  ota_init(void);

// 查询当前活跃槽位（启动时 bootloader 用）
// slot: 输出 'a' 或 'b'，失败返回 NULL
int  ota_get_active_slot(char *slot);

// 查询非活跃槽位（Commit 时写到这个槽）
int  ota_get_inactive_slot(char *slot);

// 擦除暂存区 — 创建文件，全填 0xFF（模拟 Flash 擦除）
// 对应 UDS 例程 0xFF00
int  ota_prepare_staging(void);

// Commit 更新 — 暂存区复制到非活跃槽，标记 TESTING，切换 active_slot
// 对应 UDS 例程 0xFF03
int  ota_commit_update(void);

// Accept 更新 — 新固件启动成功后，标记当前槽为 OK
// 对应 UDS 例程 0xFF04
int  ota_accept_update(void);

// 打印当前 OTA 状态（调试用）
void ota_print_status(void);

#endif // OTA_MANAGER_H