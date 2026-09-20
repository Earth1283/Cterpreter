#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int collatz(int n) {
    int steps = 0;
    while (n != 1 && steps < 400) {
        printf("%d ", n);
        n = n % 2 ? 3 * n + 1 : n / 2;
        steps++;
    }
    printf("1\n");
    return steps;
}

int longest_collatz(int limit) {
    int best = 1, best_length = 0;
    for (int start = 1; start < limit; start++) {
        long value = start;
        int length = 0;
        while (value != 1 && length < 1000) {
            value = value % 2 ? 3 * value + 1 : value / 2;
            length++;
        }
        if (length > best_length) { best_length = length; best = start; }
    }
    printf("%d takes %d steps\n", best, best_length);
    return best;
}

void roman(int n) {
    static const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char *const symbols[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    if (n < 1 || n > 3999) { printf("the Romans decline\n"); return; }
    for (int i = 0; i < 13; i++)
        while (n >= values[i]) { printf("%s", symbols[i]); n -= values[i]; }
    printf("\n");
}

void bits(long value) {
    int started = 0;
    for (int bit = 63; bit >= 0; bit--) {
        int on = (value >> bit) & 1;
        if (on) started = 1;
        if (started) putchar(on ? '1' : '0');
        if (started && bit % 8 == 0 && bit) putchar(' ');
    }
    printf("%s\n", started ? "" : "0");
}

unsigned long hash(const char *text) {
    unsigned long value = 5381UL;
    while (*text) value = value * 33UL + (unsigned char)*text++;
    return value;
}

int hanoi(int disks, char from, char to, char spare) {
    if (disks <= 0) return 0;
    int moves = hanoi(disks - 1, from, spare, to);
    printf("%c -> %c\n", from, to);
    return moves + 1 + hanoi(disks - 1, spare, to, from);
}

int primes(int limit) {
    int count = 0;
    for (int n = 2; n < limit; n++) {
        int prime = 1;
        for (int d = 2; d * d <= n; d++)
            if (n % d == 0) { prime = 0; break; }
        if (prime) { printf("%d ", n); count++; }
    }
    printf("\n%d primes below %d\n", count, limit);
    return count;
}

void bar(const char *label, int value) {
    printf("%-12s |", label);
    for (int i = 0; i < value && i < 60; i++) putchar('#');
    printf(" %d\n", value);
}

void mandel(double cx, double cy, double span) {
    const int width = 70, height = 26;
    int depth = 40 + (int)(40.0 * log(3.0 / (span > 0.0 ? span : 3.0)));
    if (depth < 40) depth = 40;
    if (depth > 250) depth = 250;
    const char *shades = " .:-=+*#%@";
    for (int row = 0; row < height; row++) {
        char line[71];
        for (int column = 0; column < width; column++) {
            double x0 = cx + span * ((double)column / width - 0.5);
            double y0 = cy + span * ((double)row / height - 0.5) * 0.5;
            double x = 0.0, y = 0.0;
            int i = 0;
            while (x * x + y * y <= 4.0 && i < depth) {
                double next = x * x - y * y + x0;
                y = 2.0 * x * y + y0;
                x = next;
                i++;
            }
            line[column] = i == depth ? '@' : shades[i * 9 / depth];
        }
        line[width] = '\0';
        puts(line);
    }
}

void bifurcation(void) {
    const int width = 76, height = 26;
    char plot[26 * 76];
    memset(plot, ' ', sizeof plot);
    for (int column = 0; column < width; column++) {
        double r = 2.8 + 1.2 * column / width;
        double x = 0.5;
        for (int i = 0; i < 300; i++) x = r * x * (1.0 - x);
        for (int i = 0; i < 200; i++) {
            x = r * x * (1.0 - x);
            int row = (int)((1.0 - x) * (height - 1));
            if (row >= 0 && row < height) plot[row * width + column] = '.';
        }
    }
    for (int row = 0; row < height; row++) {
        char line[77];
        memcpy(line, plot + row * width, width);
        line[width] = '\0';
        puts(line);
    }
    printf("r from 2.8 to 4.0, x from 1.0 down to 0.0\n");
}

int ackermann(int m, int n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

int bogosort(int count, unsigned seed) {
    int values[8];
    if (count > 8) count = 8;
    srand(seed);
    for (int i = 0; i < count; i++) values[i] = rand() % 90 + 10;
    for (int attempt = 1; attempt < 2000000; attempt++) {
        int sorted = 1;
        for (int i = 1; i < count; i++)
            if (values[i - 1] > values[i]) { sorted = 0; break; }
        if (sorted) {
            for (int i = 0; i < count; i++) printf("%d ", values[i]);
            printf("\nsorted after %d shuffles\n", attempt);
            return attempt;
        }
        for (int i = count - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int swap = values[i];
            values[i] = values[j];
            values[j] = swap;
        }
    }
    printf("gave up\n");
    return -1;
}

int main(void) {
    printf("Loaded. Things to try:\n\n");
    printf("  collatz(27)                  hailstones, and how many\n");
    printf("  longest_collatz(10000)       the stubbornest start below a limit\n");
    printf("  roman(1987)                  MCMLXXXVII\n");
    printf("  bits(1234567)                the number, in binary, byte grouped\n");
    printf("  hash(\"cterpreter\")           djb2, as an unsigned long\n");
    printf("  hanoi(4, 'A', 'C', 'B')      every move, and the count\n");
    printf("  primes(200)                  every prime below a limit\n");
    printf("  bar(\"coffee\", 17)            one row of a histogram\n");
    printf("  mandel(-0.75, 0.0, 3.0)      then zoom: mandel(-0.745, 0.113, 0.02)\n");
    printf("  bifurcation()                the logistic map losing its mind\n");
    printf("  ackermann(2, 3)              fine. ackermann(3, 6) is not\n");
    printf("  bogosort(6, 1)               sorting by pure hope\n");
    return 0;
}
