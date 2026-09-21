#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int sum(int count, ...) {
    va_list args;
    va_start(args, count);
    int total = 0;
    for (int i = 0; i < count; ++i) total += va_arg(args, int);
    va_end(args);
    return total;
}

/* Copy the list before the sizing pass: v*printf consumes the supplied list. */
int report(char *buffer, size_t capacity, const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    int written = vsnprintf(buffer, capacity, format, args);
    va_end(args);
    return needed == written ? written : -1;
}

int scan(const char *text, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int matched = vsscanf(text, format, args);
    va_end(args);
    return matched;
}

typedef struct { int x, y; } Point;

Point sum_points(int count, ...) {
    va_list args;
    va_start(args, count);
    Point total = {0, 0};
    for (int i = 0; i < count; ++i) {
        Point point = va_arg(args, Point);
        total.x += point.x;
        total.y += point.y;
    }
    va_end(args);
    return total;
}

int main(void) {
    int (*add)(int, ...) = sum;
    printf("sum = %d\n", add(4, (char)2, (short)10, 20, 10));
    char buffer[64];
    if (report(buffer, sizeof(buffer), "%s: %.2f / %lld", "promoted float", 1.25f, 42ll) < 0) return 1;
    puts(buffer);
    int number = 0;
    double fraction = 0;
    char word[16];
    int matched = scan("42 2.5 arguments", "%d %lf %15s", &number, &fraction, word);
    printf("scan = %d: %d %.1f %s\n", matched, number, fraction, word);
    Point result = sum_points(2, (Point){2, 3}, (Point){5, 8});
    printf("points = (%d, %d)\n", result.x, result.y);
    return matched != 3 || strcmp(word, "arguments") != 0 || result.x != 7 || result.y != 11;
}
