#include <stdio.h>
#define N 120
int main(void) {
    static int a[N * N], b[N * N], c[N * N];
    for (int i = 0; i < N * N; i++) {
        a[i] = i % 17; b[i] = i % 13; c[i] = 0;
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            for (int k = 0; k < N; k++)
                c[i * N + j] += a[i * N + k] * b[k * N + j];
    long long sum = 0;
    for (int i = 0; i < N * N; i++) sum += c[i];
    printf("%lld\n", sum);
    return 0;
}
