#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int append(int **values, int *length, int *capacity, int value) {
    if (*length == *capacity) {
        int grown_capacity = *capacity ? *capacity * 2 : 4;
        int *grown = realloc(*values, grown_capacity * sizeof(int));
        if (!grown) return 0;
        *values = grown;
        *capacity = grown_capacity;
    }
    (*values)[*length] = value;
    ++*length;
    return 1;
}

void erase(int *values, int *length, int index) {
    if (index < 0 || index >= *length) return;
    int remaining = *length - index - 1;
    memmove(values + index, values + index + 1, remaining * sizeof(int));
    --*length;
}

int main(void) {
    int *values = NULL;
    int length = 0;
    int capacity = 0;

    for (int i = 0; i < 10; ++i) {
        if (!append(&values, &length, &capacity, i * i)) {
            free(values);
            return 1;
        }
    }
    erase(values, &length, 3);
    erase(values, &length, 0);

    printf("length=%d capacity=%d\n", length, capacity);
    for (int i = 0; i < length; ++i) printf("%s%d", i ? " " : "", values[i]);
    putchar('\n');
    free(values);
    return 0;
}
