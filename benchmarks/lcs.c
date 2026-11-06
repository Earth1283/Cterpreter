#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;
int main(void) {
    int n = 2000;
    char *x = malloc(n + 1), *y = malloc(n + 1);
    int *prev = calloc(n + 1, sizeof(int)), *cur = calloc(n + 1, sizeof(int));
    u32 s = 99;
    for (int i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u; x[i] = (char)('a' + ((s >> 16) & 3));
        s = s * 1103515245u + 12345u; y[i] = (char)('a' + ((s >> 16) & 3));
    }
    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= n; j++) {
            if (x[i - 1] == y[j - 1]) cur[j] = prev[j - 1] + 1;
            else cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
        }
        int *t = prev; prev = cur; cur = t;
    }
    printf("%d\n", prev[n]);
    return 0;
}
