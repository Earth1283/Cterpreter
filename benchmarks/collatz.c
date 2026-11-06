#include <stdio.h>
int main(void) {
    int best = 0, arg = 0;
    long long total = 0;
    for (int n = 1; n < 150000; n++) {
        long long x = n;
        int steps = 0;
        while (x != 1) {
            if (x % 2 == 0) x = x / 2;
            else x = 3 * x + 1;
            steps++;
        }
        total += steps;
        if (steps > best) { best = steps; arg = n; }
    }
    printf("%d %d %lld\n", arg, best, total);
    return 0;
}
