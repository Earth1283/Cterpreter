#include "boot.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

__attribute__((target("avx512f"))) static int zmm_agrees(void) {
    int lanes[16];
    _mm512_storeu_si512(lanes, _mm512_add_epi32(_mm512_set1_epi32(2), _mm512_set1_epi32(2)));
    for (int i = 0; i < 16; ++i) if (lanes[i] != 4) return 0;
    return 1;
}
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

static int neon_agrees(void) {
    int lanes[4];
    vst1q_s32(lanes, vaddq_s32(vdupq_n_s32(2), vdupq_n_s32(2)));
    for (int i = 0; i < 4; ++i) if (lanes[i] != 4) return 0;
    return 1;
}
#endif

static void ceremonial_nop(void) {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__))
    __asm__ __volatile__("nop");
#endif
}

BootCheck boot_verify(void) {
    ceremonial_nop();
    volatile int two = 2;
#if defined(__x86_64__) || defined(__i386__)
    if (__builtin_cpu_supports("avx512f")) return (BootCheck){"ZMM registers", zmm_agrees()};
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return (BootCheck){"NEON registers", neon_agrees()};
#endif
    return (BootCheck){"the scalar unit", two + two == 4};
}
