#include "doip_server.h"
#include "ota_manager.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static doip_server_t g_server;

static void signal_handler(int sig) {
    printf("\n收到信号 %d，正在停止...\n", sig);
    doip_server_stop(&g_server);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;  // 消除 unused 参数警告

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    if (doip_server_init(&g_server) < 0) {
        fprintf(stderr, "服务器初始化失败\n");
        return EXIT_FAILURE;
    }

    ota_init();
    ota_print_status();

    g_server.running = 1;
    doip_server_run(&g_server);

    printf("DoIP Server 退出\n");
    return EXIT_SUCCESS;
}