#include <stdio.h>
int main(void) {
    int a[48 * 48], b[48 * 48], c[48 * 48];
    for (int i = 0; i < 48 * 48; i++) {
        a[i] = i % 17; b[i] = i % 13; c[i] = 0;
    }
    for (int i = 0; i < 48; i++)
        for (int j = 0; j < 48; j++)
            for (int k = 0; k < 48; k++)
                c[i * 48 + j] += a[i * 48 + k] * b[k * 48 + j];
    int sum = 0;
    for (int i = 0; i < 48 * 48; i++) sum += c[i];
    printf("%d\n", sum);
    return 0;
}
