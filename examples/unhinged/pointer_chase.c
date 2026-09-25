#include <stdio.h>

/* Forty-five rooms are shuffled into one long chain. Each room holds the
 * address of the next, and the last holds the address of the treasure.
 * Following the chain by hand takes a loop; following it the other way takes
 * a pointer with forty-six stars. */
#define ROOMS 45

static void *room[ROOMS];
static int treasure = 42;

static unsigned state = 2463534242u;

static unsigned shuffle_draw(unsigned bound) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state % bound;
}

int main(void) {
    int order[ROOMS];
    for (int i = 0; i < ROOMS; i++) order[i] = i;
    for (int i = ROOMS - 1; i > 0; i--) {
        int j = (int)shuffle_draw((unsigned)i + 1);
        int swap = order[i];
        order[i] = order[j];
        order[j] = swap;
    }
    for (int i = 0; i + 1 < ROOMS; i++) room[order[i]] = &room[order[i + 1]];
    room[order[ROOMS - 1]] = &treasure;

    printf("the chase:");
    void **cursor = &room[order[0]];
    for (int hop = 0; hop < ROOMS; hop++) {
        printf(" %d", (int)(cursor - room));
        cursor = (void **)*cursor;
        if (hop % 15 == 14 && hop + 1 < ROOMS) printf("\n          ");
    }
    printf(" -> treasure %d\n", *(int *)cursor);

    int **********************************************px = (int **********************************************)&room[order[0]];
    printf("%d stars later: %d\n", ROOMS + 1, **********************************************px);
    return 42 - **********************************************px;
}
