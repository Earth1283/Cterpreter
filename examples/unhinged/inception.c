#include <stdio.h>

/* Computes a factorial one level per interpreter: each level runs this same
 * file in a nested interpreter and gets the smaller factorial back as that
 * program's exit status, which is why the answer has to stay below 256. */
#define DREAMS 5

int main(void) {
#ifdef __CTERPRETER__
    int depth = interpret_depth();
    int n = DREAMS - depth;
    if (!n) {
        printf("%*slevel %d: I need 0!, and nobody dreams deeper than this. It is 1. Kick.\n", depth * 4, "", depth);
        return 1;
    }
    printf("%*slevel %d: I need %d!, so I will dream of %d!\n", depth * 4, "", depth, n, n - 1);
    int below = interpret(__FILE__);
    printf("%*slevel %d: woke up holding %d, so %d! = %d\n", depth * 4, "", depth, below, n, n * below);
    if (depth) return n * below;
    printf("%d interpreters deep and back: %d! = %d\n", DREAMS + 1, DREAMS, n * below);
#else
    puts("A compiled program has nowhere to dream. Run this under Cterpreter.");
#endif
    return 0;
}
