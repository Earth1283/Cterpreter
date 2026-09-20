#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SAMPLE_COUNT 5

static unsigned long djb2(const char *text) {
    unsigned long value = 5381UL;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; cursor++)
        value = value * 33UL + *cursor;
    return value;
}

static unsigned int checksum(const unsigned char *bytes, size_t count) {
    unsigned int total = 0u;
    for (size_t i = 0; i < count; i++) total = (total << 3) ^ (total >> 29) ^ bytes[i];
    return total;
}

int main(void) {
    printf("sizes %zu %zu %zu %zu %zu %zu %zu %zu\n",
           sizeof(_Bool), sizeof(char), sizeof(short), sizeof(int),
           sizeof(long), sizeof(long long), sizeof(float), sizeof(double));
    printf("limits %d %d %u %ld %lu\n", INT_MIN, INT_MAX, UINT_MAX, LONG_MAX, ULONG_MAX);

    unsigned int counter = 0u;
    counter -= 1u;
    printf("unsigned wraps to %u and back to %u\n", counter, counter + 1u);

    short edge = SHRT_MAX;
    printf("short promotes to %d but truncates to %d\n", edge + 1, (short)(edge + 1));

    signed char levels[SAMPLE_COUNT] = {SCHAR_MIN, -1, 0, 1, SCHAR_MAX};
    int total = 0;
    for (size_t i = 0; i < SAMPLE_COUNT; i++) total += levels[i];
    printf("signed char total %d\n", total);

    long long scaled = 1LL << 40;
    printf("long long %lld halved %lld\n", scaled, scaled >> 1);
    printf("mixed comparison %d then %d\n", -1 < 1u, -1L < 1u);

    float single = 1.0f / 3.0f;
    double wide = 1.0 / 3.0;
    printf("float %.7f double %.15f differ %d\n", single, wide, (double)single != wide);

    uint32_t mask = 0xdeadbeefU;
    printf("mask %08x rotated %08x\n", mask, (mask >> 16) | (mask << 16));

    unsigned char bytes[4] = {0x1f, 0x2e, 0x3d, 0x4c};
    printf("checksum %u hash %lu\n", checksum(bytes, sizeof bytes), djb2("Cterpreter"));

    bool ready = counter != 0u;
    printf("bool %d in %zu byte\n", ready, sizeof ready);
    return 0;
}
