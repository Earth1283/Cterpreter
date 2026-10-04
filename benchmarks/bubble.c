#include <stdio.h>
int main(void) {
    int a[900];
    for (int i = 0; i < 900; i++) a[i] = (i * 7919 + 123) % 1009;
    for (int i = 0; i < 899; i++) {
        for (int j = 0; j < 899 - i; j++) {
            if (a[j] > a[j + 1]) {
                int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
            }
        }
    }
    int sum = 0;
    for (int i = 0; i < 900; i++) sum += (i % 31) * a[i];
    printf("%d\n", sum);
    return 0;
}
