#include <stdio.h>
#include <stdlib.h>
int main(void) {
    int n = 250000, count = 0;
    unsigned char *flags = calloc(n, 1);
    for (int i = 2; i * i < n; i++) {
        if (!flags[i]) {
            for (int j = i * i; j < n; j += i) flags[j] = 1;
        }
    }
    for (int i = 2; i < n; i++) if (!flags[i]) count++;
    printf("%d\n", count);
    free(flags);
    return 0;
}
