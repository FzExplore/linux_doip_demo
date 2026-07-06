/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-06-16
****Description: 模拟车灯进程 — 定时上报 DTC，测试多进程共享
****
****    用法:  ./dtc_simulator
****    与 doip_server 同时跑，操作同一个 /tmp/dtc_store.bin
****FilePath: \demo\dtc_simulator.c
********************************************************************************/
#include "dtc_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    printf("[模拟器] 车灯控制进程启动，每 3 秒上报一个 DTC\n");
    printf("[模拟器] 按 Ctrl+C 退出\n\n");

    int step = 0;

    while (1) {
        step++;

        switch (step) {
        case 1:
            printf("[模拟器] 上报: 车灯短路 → 0xB10105\n");
            dtc_store_report(0xB10105, DTC_STATUS_CONFIRMED_DTC);
            break;

        case 2:
            printf("[模拟器] 上报: 转向灯开路 → 0xB10206\n");
            dtc_store_report(0xB10206, DTC_STATUS_PENDING_DTC);
            break;

        case 3:
            printf("[模拟器] 上报: 刹车灯过温 → 0xB10307\n");
            dtc_store_report(0xB10307, DTC_STATUS_CONFIRMED_DTC
                                       | DTC_STATUS_TEST_FAILED);
            break;

        case 4:
            printf("[模拟器] 重复上报: 车灯短路 (occurrence 应 +1)\n");
            dtc_store_report(0xB10105, DTC_STATUS_CONFIRMED_DTC);
            break;

        case 5:
            printf("[模拟器] 上报: 日行灯通信丢失 → 0xB10408\n");
            dtc_store_report(0xB10408, DTC_STATUS_CONFIRMED_DTC);
            break;

        case 6:
            printf("[模拟器] 故障清除: 车灯短路已修复 → 清除 0xB10105 当前故障位\n");
            dtc_store_clear_fault(0xB10105);
            break;

        case 7:
            printf("[模拟器] 故障清除: 刹车灯过温已恢复 → 清除 0xB10307 当前故障位\n");
            dtc_store_clear_fault(0xB10307);
            break;

        case 8:
            printf("[模拟器] 演示结束，进程保持运行。\n");
            printf("  测试提示:\n");
            printf("    19 0A     → 查看支持哪些 DTC (12 个)\n");
            printf("    19 02 08  → 查看已确认故障 (bit3=1 的仍在)\n");
            printf("    19 02 01  → 查看当前故障 (bit0=1 的已被清除)\n");
            printf("    14 FF FF FF → 清除所有 DTC (bit3 也清)\n");
            break;
        }

        // 打印当前所有 DTC
        dtc_record_t records[DTC_MAX_COUNT];
        int count = dtc_store_read_all(records, DTC_MAX_COUNT);
        printf("[模拟器] 当前 DTC 总数: %d\n", count);
        for (int i = 0; i < count; i++) {
            printf("  %02d. DTC=0x%06X  status=0x%02X  cnt=%u\n",
                   i + 1, records[i].dtc_code,
                   records[i].status_mask, records[i].occurrence);
        }
        printf("\n");

        sleep(3);
    }

    return 0;
}