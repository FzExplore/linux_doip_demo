/*******************************************************************************
****Author: Fang Zheng
****Date: 2026-07-03 12:44:48
****LastEditors: Do not edit
****LastEditTime: 2026-07-03 12:45:03
****Description: 
****FilePath: \demo\test_appB.c
********************************************************************************/
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int count = 0;
    while (1) {
        printf("[appB] 我叫 AppB, count=%d\n", count++);
        fflush(stdout);
        sleep(1);
    }
    return 0;
}