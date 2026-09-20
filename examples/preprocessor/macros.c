#include <stdio.h>
#include "config.h"
#include "config.h"

#define JOIN(left, right) left##right
#define STRINGIFY(value) #value
#define TRACE(format, ...) printf(format, __VA_ARGS__)
#define OFFSET 1
#undef OFFSET
#define OFFSET 0

int main(void) {
    int JOIN(an, swer) = SCALE(START_VALUE) + OFFSET;
#if defined(ENABLE_TRACE) && ENABLE_TRACE
    TRACE("%s = %d\n", STRINGIFY(answer), answer);
#else
    puts("Tracing disabled.");
#endif
    printf("Nested expansion: %d\n", SCALE(SCALE(1)));
    return answer != 42;
}
