#include <stdio.h>
int main(void) {
    int sum = 0;
    for (int y = 0; y < 120; y++) {
        for (int x = 0; x < 160; x++) {
            double cr = x * 3.0 / 160 - 2.0, ci = y * 2.0 / 120 - 1.0;
            double zr = 0.0, zi = 0.0;
            int n = 0;
            while (zr * zr + zi * zi < 4.0 && n < 80) {
                double next = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci; zr = next; n++;
            }
            sum += n;
        }
    }
    printf("%d\n", sum);
    return 0;
}
