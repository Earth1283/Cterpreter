#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int limit = argc > 1 ? atoi(argv[1]) : 100;
    if (limit < 2 || limit > 50000) {
        fprintf(stderr, "Limit must be between 2 and 50000.\n");
        return 2;
    }
    int *composite = calloc(limit + 1, sizeof(int));
    if (!composite) return 1;

    for (int prime = 2; prime * prime <= limit; ++prime) {
        if (composite[prime]) continue;
        for (int multiple = prime * prime; multiple <= limit; multiple += prime) composite[multiple] = 1;
    }

    int count = 0;
    for (int number = 2; number <= limit; ++number) {
        if (composite[number]) continue;
        printf("%s%d", count ? " " : "", number);
        ++count;
    }
    printf("\n%d primes through %d\n", count, limit);
    free(composite);
    return 0;
}
