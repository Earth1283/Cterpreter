#include <stdio.h>
int main(void) {
    long long total = 0;
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 320; x++) {
            double cr = x * 3.0 / 320 - 2.0, ci = y * 2.0 / 240 - 1.0;
            double zr = 0.0, zi = 0.0;
            int n = 0;
            while (zr * zr + zi * zi < 4.0 && n < 200) {
                double nxt = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = nxt;
                n++;
            }
            total += n;
        }
    }
    printf("%lld\n", total);
    return 0;
}
