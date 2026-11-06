#include <stdio.h>
typedef unsigned int u32;
int main(void) {
    u32 h = 2166136261u, s = 1;
    for (int i = 0; i < 3000000; i++) {
        s = s * 1664525u + 1013904223u;
        h ^= (s >> 24) & 255u;
        h *= 16777619u;
        h ^= h >> 13;
        h += (u32)(i & 1023);
    }
    printf("%u %u\n", h, s);
    return 0;
}
