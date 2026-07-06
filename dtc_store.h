/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-16
****Description: DTC 故障码存储模块 — 基于文件 + flock，支持多进程共享
****FilePath: \demo\dtc_store.h
********************************************************************************/
#ifndef DTC_STORE_H
#define DTC_STORE_H

#include <stdint.h>

// ============================================================
//  路径与常量
// ============================================================
#define DTC_STORE_PATH      "./data/dtc_store.bin"    // DTC 存储文件
#define DTC_SNAPSHOT_PATH    "./data/dtc_snapshot.bin"  // 快照存储文件
#define DTC_MAX_COUNT        32                      // 最多存储 32 个 DTC

// ============================================================
//  DTC 记录结构（每条 12 字节，与 DTC 文件格式一致）
// ============================================================
typedef struct {
    uint32_t dtc_code;          // DTC 码（3 字节有效，高字节 0）
    uint8_t  status_mask;       // 状态掩码（ISO 14229-1 附录 D）
    uint8_t  reserved[3];       // 对齐到 12 字节（可选存快照 ID）
    uint32_t occurrence;        // 发生次数
} dtc_record_t;

// ============================================================
//  快照记录结构（每条 16 字节）— 故障发生时的现场数据
// ============================================================
typedef struct {
    uint32_t dtc_code;          // 关联的 DTC 码
    uint8_t  record_number;     // 快照编号（1~N）
    uint8_t  reserved[3];
    uint32_t battery_voltage;   // 电池电压 (mV)
    uint32_t temperature;       // 温度 (°C * 100)
    uint32_t mileage;           // 里程 (km)
} dtc_snapshot_t;

// ============================================================
//  DTC 状态掩码位定义（ISO 14229-1 附录 D.1）
// ============================================================
#define DTC_STATUS_TEST_FAILED          0x01   // bit0: 当前故障已确认
#define DTC_STATUS_TEST_FAILED_THIS_OP  0x02   // bit1: 当前操作循环故障
#define DTC_STATUS_PENDING_DTC          0x04   // bit2: 待确认故障
#define DTC_STATUS_CONFIRMED_DTC        0x08   // bit3: 已确认故障
#define DTC_STATUS_TEST_NOT_COMPLETE    0x10   // bit4: 测试未完成
#define DTC_STATUS_TEST_FAILED_SINCE_CLR 0x20  // bit5: 清除后故障
#define DTC_STATUS_TEST_NOT_COMPLETE_SINCE_CLR 0x40  // bit6: 清除后测试未完成
#define DTC_STATUS_WARNING_INDICATOR     0x80  // bit7: 警告指示灯请求

// ============================================================
//  API
// ============================================================

// 添加或更新一个 DTC（会自动去重，合并 occurrence）
int dtc_store_report(uint32_t dtc_code, uint8_t status_mask);

// 故障消失：清除当前故障位（bit0/bit1/bit2），保留已确认位（bit3/bit5）
int dtc_store_clear_fault(uint32_t dtc_code);

// 读取所有 DTC（返回实际读到的条数）
int dtc_store_read_all(dtc_record_t *records, int max_count);

// 按状态掩码过滤 DTC（返回符合条件的条数）
int dtc_store_read_by_status(uint8_t status_mask, dtc_record_t *records, int max_count);

// 按状态掩码计数（返回符合条件的 DTC 数量）
int dtc_store_count_by_status(uint8_t status_mask);

// 获取 ECU 支持的所有 DTC 码（预定义列表，非当前发生的）
int dtc_store_get_supported(dtc_record_t *records, int max_count);

// 记录快照 — 故障发生时调用，自动捕获当前模拟数据
int dtc_store_snapshot_capture(uint32_t dtc_code);

// 读取指定 DTC 的快照记录
int dtc_store_snapshot_read(uint32_t dtc_code, uint8_t record_number,
                            dtc_snapshot_t *snapshot);

// 清除所有 DTC
int dtc_store_clear_all(void);

// 清除指定 DTC 组（group_mask 非 0xFF 时按高 2 字节匹配）
int dtc_store_clear_group(uint32_t dtc_group_mask);

#endif // DTC_STORE_H