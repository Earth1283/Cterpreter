#include "cterpreter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(CtInterpreter *interpreter, const char *source, CtStatus expected, int value) {
    CtError error;
    CtValue result;
    int has_value;
    CtStatus status = ct_eval(interpreter, source, &result, &has_value, &error);
    if (status != expected || (status == CT_OK && (!has_value || result.type != CT_INT || result.as.integer != value))) {
        fprintf(stderr, "Failed: %s\nstatus=%d, diagnostic=%s\n", source, status, error.message);
        exit(EXIT_FAILURE);
    }
}

int main(void) {
    CtInterpreter *a = ct_create(), *b = ct_create();
    if (!a || !b) return EXIT_FAILURE;
    check(a, "2 + 3 * 4", CT_OK, 14);
    check(a, "int x = 7; x", CT_OK, 7);
    check(a, "x += 3; x", CT_OK, 10);
    check(a, "x = 99; (", CT_INCOMPLETE, 0);
    check(a, "x", CT_OK, 10);
    check(a, "0 && (x = 99); 1 || (1 / 0)", CT_OK, 1);
    check(a, "{ int x = 3; x += 1; } x", CT_OK, 10);
    check(a, "int sum = 0; for (int i = 0; i < 10; i++) { if (i == 3) continue; sum += i; } sum", CT_OK, 42);
    check(a, "int fact(int n) { if (n < 2) return 1; return n * fact(n - 1); } fact(6)", CT_OK, 720);
    check(a, "fact(5)", CT_OK, 120);
    check(a, "1 / 0", CT_ERROR, 0);
    check(a, "2147483647 + 1", CT_ERROR, 0);
    check(a, "for (;;) {}", CT_ERROR, 0);
    check(a, "x", CT_OK, 10);
    check(b, "x", CT_ERROR, 0);
    check(a, "int values[] = {3,4,5}; int *p = values; p[1] += 2; values[1]", CT_OK, 6);
    check(a, "int *heap = calloc(2, sizeof(int)); heap[1]=9; heap[0]+heap[1]", CT_OK, 9);
    check(a, "free(heap); heap[0]", CT_ERROR, 0);
    check(a, "int *expired; { int local=1; expired=&local; } *expired", CT_ERROR, 0);
    check(a, "values[3]", CT_ERROR, 0);
    check(a, "int counter(void) { static int n=0; return ++n; } counter()", CT_OK, 1);
    check(a, "counter()", CT_OK, 2);
    check(a, "typedef int Count; enum Kind { FIRST=4, SECOND }; Count count=SECOND; count", CT_OK, 5);
    check(a, "Count next=count+1; _Generic(next++, int: next, default: 0)", CT_OK, 6);
    check(a, "int n=0; do { n++; } while(n<3); switch(n) { case 3: n+=2; break; default: n=0; } n", CT_OK, 5);
    check(a, "#define SUM(a,b) ((a)+(b))\n#if defined(SUM)\nSUM(3,4)\n#endif", CT_OK, 7);
    check(a, "SUM(2,6)", CT_OK, 8);
    check(a, "char text[8]; snprintf(text,sizeof(text),\"%s\",\"hello\"); strcmp(text,\"hello\")", CT_OK, 0);
    check(a, "int parsed; sscanf(\"42\",\"%d\",&parsed); parsed", CT_OK, 42);
    FILE *output = tmpfile();
    if (!output) return EXIT_FAILURE;
    ct_set_streams(a, NULL, output, NULL);
    check(a, "printf(\"%s %04d %.2f\\n\", \"ok\", 7, 2.5); 1", CT_OK, 1);
    rewind(output);
    char text[64];
    if (!fgets(text, sizeof text, output) || strcmp(text, "ok 0007 2.50\n")) return EXIT_FAILURE;
    ct_set_streams(a, NULL, NULL, NULL);
    fclose(output);
    check(a, "struct Point { int x, y; }; struct Point corner = {3, 4}; corner.x + corner.y", CT_OK, 7);
    check(a, "struct Point *at = &corner; at->y = 9; corner.y", CT_OK, 9);
    check(a, "struct Point moved = corner; moved.x = 0; corner.x + moved.x", CT_OK, 3);
    check(a, "struct Point shift(struct Point s) { s.x += 1; return s; } shift(corner).x", CT_OK, 4);
    check(a, "union Word { int number; char bytes[4]; } w; w.number = 0; w.bytes[0] = 1; w.number", CT_OK, 1);
    check(a, "int grid[2][3] = {{1,2,3},{4,5,6}}; grid[1][2] + (int)sizeof(grid[0])", CT_OK, 6 + 3 * (int)sizeof(int));
    check(a, "int twice(int n) { return n * 2; } int (*call)(int) = twice; call(21)", CT_OK, 42);
    check(a, "int sparse[5] = { [4] = 7, [0] = 1 }; sparse[0] + sparse[4]", CT_OK, 8);
    check(a, "struct Point named = { .y = 5 }; named.x + named.y", CT_OK, 5);
    check(a, "(struct Point){6, 7}.y", CT_OK, 7);
    check(a, "void scoped(void) { typedef int Local; Local value = 3; } (int)sizeof(struct Point)", CT_OK, 2 * (int)sizeof(int));
    check(a, "corner.missing", CT_ERROR, 0);
    check(a, "struct Point unassignable; int bad = unassignable;", CT_ERROR, 0);
    check(a, "#include <stdbool.h>\nbool flag = true; flag && __STDC__", CT_OK, 1);
    check(a, "#include <stddef.h>\n(int)offsetof(struct Point, y)", CT_OK, (int)sizeof(int));
    check(a, "int order(const void *l, const void *r) { return *(const int *)l - *(const int *)r; }"
             "int list[4] = {4,2,3,1}; qsort(list, 4, sizeof(int), order); list[0] * 1000 + list[3]", CT_OK, 1004);
    check(a, "#include <errno.h>\nerrno = EDOM; errno == EDOM", CT_OK, 1);
    ct_clear(a);
    check(a, "x", CT_ERROR, 0);
    ct_destroy(a);
    ct_destroy(b);
    return EXIT_SUCCESS;
}
