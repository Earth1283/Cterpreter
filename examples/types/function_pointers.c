#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Operation {
    char name[8];
    int (*apply)(int, int);
};

static int add(int a, int b) { return a + b; }
static int subtract(int a, int b) { return a - b; }
static int multiply(int a, int b) { return a * b; }

static int fold(const struct Operation *operation, const int *values, int count, int seed) {
    int result = seed;
    for (int i = 0; i < count; i++) result = operation->apply(result, values[i]);
    return result;
}

static int descending(const void *left, const void *right) {
    return *(const int *)right - *(const int *)left;
}

static int by_length(const void *left, const void *right) {
    return (int)strlen(left) - (int)strlen(right);
}

int main(void) {
    struct Operation operations[3] = { {"add", add}, {"sub", subtract}, {"mul", multiply} };
    int values[4] = {1, 2, 3, 4};
    for (int i = 0; i < 3; i++)
        printf("%s -> %d\n", operations[i].name, fold(&operations[i], values, 4, i == 2 ? 1 : 0));

    int (*chosen)(int, int) = operations[1].apply;
    printf("indirect subtract %d\n", chosen(10, 4));

    int scores[6] = {42, 7, 19, 3, 88, 23};
    qsort(scores, 6, sizeof(int), descending);
    for (int i = 0; i < 6; i++) printf("%d%s", scores[i], i + 1 < 6 ? " " : "\n");

    int needle = 19;
    int *found = bsearch(&needle, scores, 6, sizeof(int), descending);
    printf("found %d at index %d\n", found ? *found : -1, found ? (int)(found - scores) : -1);

    char words[4][8] = {"delta", "a", "epsilon", "beta"};
    qsort(words, 4, sizeof words[0], by_length);
    for (int i = 0; i < 4; i++) printf("%s%s", words[i], i + 1 < 4 ? " " : "\n");
    return 0;
}
