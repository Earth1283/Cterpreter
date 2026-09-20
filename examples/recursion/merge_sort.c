#include <stdio.h>
#include <stdlib.h>

void merge(int *values, int *scratch, int begin, int middle, int end) {
    int left = begin;
    int right = middle;
    int output = begin;

    while (left < middle && right < end) {
        if (values[left] <= values[right]) scratch[output++] = values[left++];
        else scratch[output++] = values[right++];
    }
    while (left < middle) scratch[output++] = values[left++];
    while (right < end) scratch[output++] = values[right++];
    for (int i = begin; i < end; ++i) values[i] = scratch[i];
}

void merge_sort(int *values, int *scratch, int begin, int end) {
    if (end - begin < 2) return;
    int middle = begin + (end - begin) / 2;
    merge_sort(values, scratch, begin, middle);
    merge_sort(values, scratch, middle, end);
    merge(values, scratch, begin, middle, end);
}

int main(void) {
    int values[] = {38, -4, 27, 0, 43, 3, 9, 82, 10, 27, -19, 5};
    int count = sizeof(values) / sizeof(int);
    int *scratch = malloc(count * sizeof(int));
    if (!scratch) return 1;

    merge_sort(values, scratch, 0, count);
    for (int i = 0; i < count; ++i) printf("%s%d", i ? " " : "", values[i]);
    putchar('\n');
    free(scratch);
    return 0;
}
