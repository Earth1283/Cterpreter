#include <stdio.h>
#include <stdlib.h>

#define COUNT 5

int main(void) {
    int *values = calloc(COUNT, sizeof(int));
    for (int i = 0; i < COUNT; ++i) values[i] = i * i;
    for (int i = 0; i < COUNT; ++i) printf("%d%s", values[i], i + 1 == COUNT ? "\n" : ", ");
    free(values);
    return 0;
}
