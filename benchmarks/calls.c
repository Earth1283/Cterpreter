#include <stdio.h>
int mix(int x, int y) { return (x * 3 + y) % 1009; }
int main(void) {
    int sum = 0;
    for (int i = 0; i < 500000; i++) sum += mix(i % 997, i % 31);
    printf("%d\n", sum);
    return 0;
}
