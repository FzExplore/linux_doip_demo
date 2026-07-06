#include <stdio.h>
#include <unistd.h>

int main(void)
{
    int count = 0;
    while (1) {
        printf("[appA] 我叫 AppA, count=%d\n", count++);
        fflush(stdout);
        sleep(1);
    }
    return 0;
}