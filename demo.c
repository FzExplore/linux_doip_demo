#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>

volatile sig_atomic_t running = 1;

// 收到 SIGHUP 时会进这里
void on_sighup(int sig) {
    printf("\n!!! 收到了 SIGHUP 信号（终端被关了）!!!\n");
    printf("   如果不处理，默认行为就是退出程序\n");
    printf("   但我选择忽略它，继续运行...\n\n");
    // 什么都不做，继续运行
}

void on_sigterm(int sig) {
    printf("\n收到 SIGTERM，退出\n");
    running = 0;
}

void on_sigint(int sig) {
    printf("\n收到 SIGINT（Ctrl+C），退出\n");
    running = 0;
}

int main() {
    // 注册信号处理
    signal(SIGHUP,  on_sighup);   // 终端关闭信号 → 我选择忽略
    signal(SIGTERM, on_sigterm);  // kill 命令信号
    signal(SIGINT,  on_sigint);   // Ctrl+C 信号

    printf("========================================\n");
    printf(" SIGHUP 实验程序\n");
    printf(" PID = %d\n", getpid());
    printf("========================================\n");
    printf("\n");
    printf("这个程序做了什么：\n");
    printf("  1. 我捕获了 SIGHUP，选择【忽略它】\n");
    printf("  2. 所以即使你关闭终端，我也不会死\n");
    printf("\n");
    printf("实验步骤：\n");
    printf("  第一步：直接关掉这个终端窗口（或按 Ctrl+D）\n");
    printf("  第二步：重新打开一个新终端\n");
    printf("  第三步：执行  ps aux | grep sighup_demo\n");
    printf("  第四步：你还能看到我！我还活着！\n");
    printf("  第五步：kill %d  把我杀掉\n", getpid());
    printf("\n");
    printf("如果我没有忽略 SIGHUP（默认行为），\n");
    printf("你关终端的瞬间我就死了。\n");
    printf("========================================\n\n");

    int count = 0;
    while (running) {
        time_t now = time(NULL);
        char *time_str = ctime(&now);
        time_str[strlen(time_str) - 1] = '\0'; // 去掉换行
        printf("[第 %3d 次] %s  →  我还活着，PID=%d\n", ++count, time_str, getpid());
        fflush(stdout);
        sleep(2);
    }

    printf("程序退出\n");
    return 0;
}
