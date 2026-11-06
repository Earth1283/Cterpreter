#include <stdio.h>
#include <stdlib.h>
typedef unsigned int u32;
void sort(int *a, int lo, int hi) {
    while (lo < hi) {
        int pivot = a[(lo + hi) / 2], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) {
                int t = a[i]; a[i] = a[j]; a[j] = t;
                i++; j--;
            }
        }
        if (j - lo < hi - i) { sort(a, lo, j); lo = i; }
        else { sort(a, i, hi); hi = j; }
    }
}
int main(void) {
    int n = 200000;
    int *a = malloc(n * sizeof(int));
    u32 s = 12345;
    for (int i = 0; i < n; i++) {
        s = s * 1103515245u + 12345u;
        a[i] = (int)((s >> 8) & 0xFFFFF);
    }
    sort(a, 0, n - 1);
    long long total = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && a[i - 1] > a[i]) { printf("unsorted\n"); return 1; }
        total += (long long)(i % 101) * a[i];
    }
    printf("%lld\n", total);
    free(a);
    return 0;
}
