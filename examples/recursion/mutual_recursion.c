#include <stdio.h>

int is_even(int n);
int is_odd(int n);

int is_even(int n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}

int is_odd(int n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}

int main(void) {
    for (int n = 0; n <= 9; ++n) {
        printf("%d: %s\n", n, is_even(n) ? "even" : "odd");
    }
    return 0;
}
