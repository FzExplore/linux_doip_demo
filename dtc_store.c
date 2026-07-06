/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-16
****Description: DTC 存储模块实现 — 文件 + flock 多进程安全
****FilePath: \demo\dtc_store.c
********************************************************************************/
#include "dtc_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>

// ============================================================
//  内部辅助：加锁打开文件
//  mode: "rb" 读锁, "r+b" 或 "wb" 写锁
//  返回 FILE*，失败返回 NULL
// ============================================================
static FILE *dtc_fopen(const char *mode, int *fd_out) {
    int lock_type = (strchr(mode, 'w') || strchr(mode, '+'))
                    ? LOCK_EX : LOCK_SH;

    int fd = open(DTC_STORE_PATH, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return NULL;

    if (flock(fd, lock_type) < 0) {
        close(fd);
        return NULL;
    }

    FILE *f = fdopen(fd, mode);
    if (!f) {
        flock(fd, LOCK_UN);
        close(fd);
        return NULL;
    }

    *fd_out = fd;
    return f;
}

// ============================================================
//  内部辅助：解锁关闭
// ============================================================
static void dtc_fclose(FILE *f, int fd) {
    fflush(f);
    fsync(fd);                  // 防掉电
    flock(fd, LOCK_UN);
    fclose(f);                  // fclose 会自动 close(fd)
}

// ============================================================
//  添加/更新 DTC（去重，合并 occurrence）
// ============================================================
int dtc_store_report(uint32_t dtc_code, uint8_t status_mask) {
    if (dtc_code == 0 || (dtc_code & 0xFF000000)) return -1;

    int fd;
    FILE *f = dtc_fopen("r+b", &fd);
    if (!f) return -1;

    dtc_record_t rec;
    int found = 0;

    // 先查是否已存在
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.dtc_code == dtc_code) {
            rec.status_mask = status_mask;
            rec.occurrence++;
            fseek(f, -(long)sizeof(rec), SEEK_CUR);
            fwrite(&rec, sizeof(rec), 1, f);
            found = 1;
            break;
        }
    }

    // 不存在则追加 + 捕获快照
    if (!found) {
        memset(&rec, 0, sizeof(rec));
        rec.dtc_code    = dtc_code;
        rec.status_mask = status_mask;
        rec.occurrence  = 1;
        fseek(f, 0, SEEK_END);
        fwrite(&rec, sizeof(rec), 1, f);

        dtc_store_snapshot_capture(dtc_code);
    }

    dtc_fclose(f, fd);
    return 0;
}

// ============================================================
//  快照捕获 — 故障发生时自动记录现场数据
//  模拟生成电池电压、温度、里程
// ============================================================
int dtc_store_snapshot_capture(uint32_t dtc_code) {
    if (dtc_code == 0 || (dtc_code & 0xFF000000)) return -1;

    dtc_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.dtc_code       = dtc_code;
    snap.record_number  = 1;
    snap.battery_voltage = 12000 + (rand() % 2000);   // 12~14V
    snap.temperature     = 4000 + (rand() % 4000);     // 40~80°C
    snap.mileage         = rand() % 100000;            // 0~100000km

    int fd = open(DTC_SNAPSHOT_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    flock(fd, LOCK_EX);
    write(fd, &snap, sizeof(snap));
    fsync(fd);
    flock(fd, LOCK_UN);
    close(fd);
    return 0;
}

// ============================================================
//  读取指定 DTC 的快照
// ============================================================
int dtc_store_snapshot_read(uint32_t dtc_code, uint8_t record_number,
                            dtc_snapshot_t *snapshot) {
    int fd = open(DTC_SNAPSHOT_PATH, O_RDONLY);
    if (fd < 0) return 0;

    flock(fd, LOCK_SH);

    int count = 0;
    dtc_snapshot_t snap;
    while (read(fd, &snap, sizeof(snap)) == sizeof(snap)) {
        if (snap.dtc_code == dtc_code &&
            snap.record_number == record_number) {
            memcpy(snapshot, &snap, sizeof(snap));
            count = 1;
            break;
        }
    }

    flock(fd, LOCK_UN);
    close(fd);
    return count;
}

// ============================================================
//  故障消失：清除当前故障位，保留已确认位
//  bit0 (TEST_FAILED) → 0      故障条件不再满足
//  bit1 (TEST_FAILED_THIS_OP) → 0
//  bit2 (PENDING_DTC) → 0      不再待确认
//  bit3 (CONFIRMED_DTC) → 保留  已确认故障不消失
//  bit5 (TEST_FAILED_SINCE_CLR) → 保留
//  bit6 (TEST_NOT_COMPLETE_SINCE_CLR) → 0  测试已完成
// ============================================================
int dtc_store_clear_fault(uint32_t dtc_code) {
    if (dtc_code == 0 || (dtc_code & 0xFF000000)) return -1;

    int fd;
    FILE *f = dtc_fopen("r+b", &fd);
    if (!f) return -1;

    dtc_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.dtc_code == dtc_code) {
            // 清除当前故障位，保留已确认和历史位
            rec.status_mask &= ~(DTC_STATUS_TEST_FAILED
                               | DTC_STATUS_TEST_FAILED_THIS_OP
                               | DTC_STATUS_PENDING_DTC
                               | DTC_STATUS_TEST_NOT_COMPLETE_SINCE_CLR);
            fseek(f, -(long)sizeof(rec), SEEK_CUR);
            fwrite(&rec, sizeof(rec), 1, f);
            break;
        }
    }

    dtc_fclose(f, fd);
    return 0;
}

// ============================================================
//  读取所有 DTC
// ============================================================
int dtc_store_read_all(dtc_record_t *records, int max_count) {
    int fd;
    FILE *f = dtc_fopen("rb", &fd);
    if (!f) return 0;

    int count = 0;
    dtc_record_t rec;
    while (count < max_count && fread(&rec, sizeof(rec), 1, f) == 1) {
        records[count++] = rec;
    }

    dtc_fclose(f, fd);
    return count;
}

// ============================================================
//  按状态掩码过滤 DTC
// ============================================================
int dtc_store_read_by_status(uint8_t status_mask, dtc_record_t *records,
                             int max_count) {
    int fd;
    FILE *f = dtc_fopen("rb", &fd);
    if (!f) return 0;

    int count = 0;
    dtc_record_t rec;
    while (count < max_count && fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.status_mask & status_mask) {
            records[count++] = rec;
        }
    }

    dtc_fclose(f, fd);
    return count;
}

// ============================================================
//  按状态掩码计数
// ============================================================
int dtc_store_count_by_status(uint8_t status_mask) {
    int fd;
    FILE *f = dtc_fopen("rb", &fd);
    if (!f) return 0;

    int count = 0;
    dtc_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.status_mask & status_mask) {
            count++;
        }
    }

    dtc_fclose(f, fd);
    return count;
}

// ============================================================
//  ECU 支持的所有 DTC 码（预定义列表）
//  诊断仪用 0x19 0x0A 查询 "你能检测哪些故障"
//  与实际是否发生无关，始终返回此列表
// ============================================================
static const dtc_record_t supported_dtc_list[] = {
    { 0xB10105, 0x00, {0}, 0 },  // 左前大灯短路
    { 0xB10106, 0x00, {0}, 0 },  // 右前大灯短路
    { 0xB10206, 0x00, {0}, 0 },  // 左前转向灯开路
    { 0xB10207, 0x00, {0}, 0 },  // 右前转向灯开路
    { 0xB10307, 0x00, {0}, 0 },  // 刹车灯过温
    { 0xB10308, 0x00, {0}, 0 },  // 刹车灯电压异常
    { 0xB10408, 0x00, {0}, 0 },  // 日行灯通信丢失
    { 0xB10409, 0x00, {0}, 0 },  // 日行灯 LED 失效
    { 0xB10501, 0x00, {0}, 0 },  // 雾灯对地短路
    { 0xB10502, 0x00, {0}, 0 },  // 雾灯对电源短路
    { 0xB10601, 0x00, {0}, 0 },  // 倒车灯开路
    { 0xB10602, 0x00, {0}, 0 },  // 倒车灯短路
};
#define SUPPORTED_DTC_COUNT (sizeof(supported_dtc_list) / sizeof(supported_dtc_list[0]))

int dtc_store_get_supported(dtc_record_t *records, int max_count) {
    int count = (int)SUPPORTED_DTC_COUNT;
    if (count > max_count) count = max_count;
    memcpy(records, supported_dtc_list, count * sizeof(dtc_record_t));
    return count;
}

// ============================================================
//  清除所有 DTC
// ============================================================
int dtc_store_clear_all(void) {
    int fd;
    FILE *f = dtc_fopen("r+b", &fd);
    if (!f) return -1;

    ftruncate(fd, 0);  // 截断文件（fdopen 不会自动截断）

    dtc_fclose(f, fd);
    return 0;
}

// ============================================================
//  清除指定 DTC 组
// ============================================================
int dtc_store_clear_group(uint32_t dtc_group_mask) {
    int fd;
    FILE *f = dtc_fopen("r+b", &fd);
    if (!f) return -1;

    // 读取所有记录，过滤掉要清除的，写回
    dtc_record_t keep[DTC_MAX_COUNT];
    int keep_count = 0;
    dtc_record_t rec;

    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        uint32_t group = (rec.dtc_code >> 16) & 0xFF;
        if (group != (dtc_group_mask & 0xFF)) {
            keep[keep_count++] = rec;
        }
    }

    ftruncate(fd, 0);
    rewind(f);
    fwrite(keep, sizeof(dtc_record_t), keep_count, f);

    dtc_fclose(f, fd);
    return 0;
}