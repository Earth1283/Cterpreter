#include <stdio.h>

int factorial(int n) {
    if (n < 2) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    printf("10! = %d\n", factorial(10));
    return 0;
}
