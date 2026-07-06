/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-12 16:00:00
****Description: AB 分区 OTA 管理器实现
****    - 元数据原子写入（先写 .tmp 再 rename，防止断电损坏）
****    - 暂存区隔离下载过程，不污染活跃槽
****    - Commit 后标记 TESTING，需诊断仪 Accept 或超时回滚
****FilePath: \demo\ota_manager.c
********************************************************************************/
#include "ota_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

// ============================================================
//  工具函数 — CRC8 校验元数据完整性
//  多项式: 0x1D (x^8 + x^4 + x^3 + x^2 + 1)
// ============================================================
static uint8_t calc_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x1D) : (crc << 1);
    }
    return crc;
}

// ============================================================
//  读取元数据 — 带 CRC8 校验
//  返回: 0 成功且校验通过，-1 文件不存在或校验失败
// ============================================================
static int read_status(ota_status_t *st) {
    FILE *f = fopen(OTA_STATUS_FILE, "rb");
    if (!f) return -1;
    if (fread(st, 1, sizeof(*st), f) != sizeof(*st)) { fclose(f); return -1; }
    fclose(f);
    uint8_t crc = st->crc8;
    st->crc8 = 0;
    return (calc_crc8((uint8_t*)st, sizeof(*st)) == crc) ? 0 : -1;
}

// ============================================================
//  原子写入元数据 — 先写临时文件，成功再 rename
//  防止写入过程中断电导致元数据损坏
// ============================================================
static int write_status(const ota_status_t *st) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", OTA_STATUS_FILE);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    ota_status_t w = *st;
    w.crc8 = 0;
    w.crc8 = calc_crc8((uint8_t*)&w, sizeof(w));
    if (fwrite(&w, 1, sizeof(w), f) != sizeof(w)) { fclose(f); return -1; }
    fclose(f);
    // rename 是原子操作：要么旧文件在，要么新文件替换完成
    return rename(tmp, OTA_STATUS_FILE);
}

// ============================================================
//  递归创建目录（类似 mkdir -p）
// ============================================================
static int mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755);
}

// ============================================================
//  原子复制文件 — src → dest（先写 .tmp 再 rename）
//  防止复制过程中断电导致目标文件损坏
// ============================================================
static int atomic_copy(const char *src_path, const char *dest_path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest_path);

    // 确保目标目录存在
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", dest_path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir_p(dir); }

    FILE *src = fopen(src_path, "rb");
    if (!src) { printf("[OTA] 原子复制失败: 源文件不存在\n"); return -1; }
    FILE *dst = fopen(tmp, "wb");
    if (!dst) { fclose(src); return -1; }

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src); fclose(dst);

    if (rename(tmp, dest_path) != 0) { printf("[OTA] 原子复制失败: rename\n"); return -1; }
    printf("[OTA] 原子复制: %s → %s\n", src_path, dest_path);
    return 0;
}

// ============================================================
//  初始化 OTA 子系统
//  1. 创建所有需要的目录
//  2. 如果元数据不存在（首次启动），初始化为 A 槽 OK
//  3. 如果元数据存在，加载并校验
// ============================================================
int ota_init(void) {
    // 创建目录树
    mkdir_p(OTA_BASE_DIR "/staging");
    mkdir_p(OTA_BASE_DIR "/slot_a");
    mkdir_p(OTA_BASE_DIR "/slot_b");

    ota_status_t st;
    if (read_status(&st) == 0) {
        // 元数据已存在，加载
        printf("[OTA] 已加载: active=%c A=%d B=%d\n",
               st.active_slot, st.slot_a_status, st.slot_b_status);
        return 0;
    }

    // 首次初始化：A 槽为 OK，B 槽为空
    memset(&st, 0, sizeof(st));
    memcpy(st.magic, "OTA!", 4);         // 写入魔数
    st.active_slot    = 'a';             // 默认从 A 槽启动
    st.slot_a_status  = SLOT_STATUS_OK;
    st.slot_b_status  = SLOT_STATUS_INACTIVE;
    st.slot_a_tries   = MAX_BOOT_TRIES;  // A 槽默认有 3 次机会
    st.slot_b_tries   = 0;               // B 槽从未尝试过
    write_status(&st);
    printf("[OTA] 首次初始化: active_slot=a\n");
    return 0;
}

// ============================================================
//  查询活跃槽（bootloader 启动时读这个决定加载哪个固件）
// ============================================================
int ota_get_active_slot(char *slot) {
    ota_status_t st;
    if (read_status(&st) != 0) return -1;
    *slot = st.active_slot;
    return 0;
}

// ============================================================
//  查询非活跃槽（Commit 时写到这个槽，防止覆盖当前运行固件）
// ============================================================
int ota_get_inactive_slot(char *slot) {
    ota_status_t st;
    if (read_status(&st) != 0) return -1;
    *slot = (st.active_slot == 'a') ? 'b' : 'a';
    return 0;
}

// ============================================================
//  擦除暂存区 — 删除旧文件，准备好接收新固件
//  对应 UDS 0x31 01 FF 00
//  注意：真实 Flash 擦除是写 0xFF，这里用文件模拟只需清空
// ============================================================
int ota_prepare_staging(void) {
    mkdir_p(OTA_BASE_DIR "/staging");
    // 删除旧暂存文件（模拟 Flash 擦除）
    remove(OTA_STAGING_FILE);
    return 0;
}

// ============================================================
//  Commit 更新 — 把暂存区固件正式安装到非活跃槽
//  1. 校验暂存区文件存在且大小合理
//  2. 确定目标槽位（非活跃槽）
//  3. 原子复制暂存区 → 非活跃槽
//  4. 非活跃槽标记为 TESTING，切换 active_slot
//  5. 诊断仪收到 71 FF 03 后发 0x11 01 复位 → 启动新固件
// ============================================================
int ota_commit_update(void) {
    // 1. 校验暂存区文件存在且大小合理
    struct stat st_src;
    if (stat(OTA_STAGING_FILE, &st_src) != 0) {
        printf("[OTA] Commit 失败: 暂存区不存在\n");
        return -1;
    }
    if (st_src.st_size <= 0) {
        printf("[OTA] Commit 失败: 暂存区为空 (0 字节)\n");
        return -1;
    }
    printf("[OTA] Commit: 暂存区大小 = %ld 字节\n", (long)st_src.st_size);

    // 2. 确定写到哪个槽
    char inactive;
    if (ota_get_inactive_slot(&inactive) != 0) return -1;

    char dest[256];
    snprintf(dest, sizeof(dest), "%s/slot_%c/firmware.bin", OTA_BASE_DIR, inactive);

    // 3. 原子复制：先写 .tmp 再 rename（防止写一半断电）
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);

    FILE *src = fopen(OTA_STAGING_FILE, "rb");
    if (!src) { printf("[OTA] Commit 失败: 无法打开暂存区\n"); return -1; }
    FILE *dst = fopen(tmp, "wb");
    if (!dst) { fclose(src); return -1; }

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src); fclose(dst);

    if (rename(tmp, dest) != 0) { printf("[OTA] Commit 失败: rename\n"); return -1; }

    // 4. 加可执行权限，否则 Boot Manager 启动不了
    chmod(dest, 0755);

    // 5. 更新元数据：非活跃槽 → TESTING，切换 active_slot
    ota_status_t st;
    if (read_status(&st) != 0) return -1;
    if (inactive == 'a') {
        st.slot_a_status = SLOT_STATUS_TESTING;
        st.slot_a_tries  = MAX_BOOT_TRIES;
    } else {
        st.slot_b_status = SLOT_STATUS_TESTING;
        st.slot_b_tries  = MAX_BOOT_TRIES;
    }
    st.active_slot = inactive;
    write_status(&st);

    printf("[OTA] Commit 成功: slot_%c → TESTING\n", inactive);
    return 0;
}

// ============================================================
//  Accept 更新 — 新固件启动成功，诊断仪确认后标记为 OK
//  对应 UDS 0x31 01 FF 04
//  1. 标记当前槽为 OK（状态机: TESTING → OK）
//  2. 将当前槽固件原子复制到供应商指定目录
//  如果不调这个，bootloader 会在尝试次数耗尽后自动回滚
// ============================================================
int ota_accept_update(void) {
    ota_status_t st;
    if (read_status(&st) != 0) return -1;

    // 1. 当前槽标记为 OK
    if (st.active_slot == 'a')
        st.slot_a_status = SLOT_STATUS_OK;
    else
        st.slot_b_status = SLOT_STATUS_OK;
    write_status(&st);
    printf("[OTA] Accept: slot_%c → OK\n", st.active_slot);

    // 2. 复制到供应商目录（SDK 要求的最终交付路径）
    char slot_path[256];
    snprintf(slot_path, sizeof(slot_path), "%s/slot_%c/firmware.bin", OTA_BASE_DIR, st.active_slot);
    if (atomic_copy(slot_path, VENDOR_UPDATE_PATH) != 0) {
        printf("[OTA] 部署到供应商目录失败!\n");
        return -1;
    }
    return 0;
}

// ============================================================
//  打印当前 OTA 状态（调试用，每次启动时打印一次）
// ============================================================
void ota_print_status(void) {
    ota_status_t st;
    if (read_status(&st) != 0) { printf("[OTA] 无状态文件\n"); return; }
    const char *names[] = {"INACTIVE", "OK", "TESTING", "FAILED"};
    printf("[OTA] active=%c | A:%s(tries=%d) | B:%s(tries=%d)\n",
           st.active_slot,
           names[st.slot_a_status], st.slot_a_tries,
           names[st.slot_b_status], st.slot_b_tries);
}