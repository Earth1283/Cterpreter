#include <stdio.h>
int main(void) {
    int x = 7, sum = 0;
    for (int i = 0; i < 1000000; i++) {
        x = (x * 17 + 23) % 1009;
        sum += (x & 255) ^ (i & 127);
    }
    printf("%d %d\n", x, sum);
    return 0;
}
