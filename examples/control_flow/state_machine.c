#include <stdio.h>

#define COFFEE_PRICE 3

enum State { WAITING, CREDIT, DISPENSING };

int main(void) {
    char events[] = "cccdbcxcdd";
    enum State state = WAITING;
    int credit = 0;
    int drinks = 0;

    for (int i = 0; events[i]; ++i) {
        char event = events[i];
        switch (event) {
            case 'c':
                ++credit;
                state = CREDIT;
                printf("coin: credit=%d\n", credit);
                break;
            case 'd':
                if (credit < COFFEE_PRICE) {
                    printf("drink refused: credit=%d\n", credit);
                    continue;
                }
                state = DISPENSING;
                credit -= COFFEE_PRICE;
                ++drinks;
                printf("dispense: drink=%d credit=%d\n", drinks, credit);
                state = credit ? CREDIT : WAITING;
                break;
            case 'x':
                printf("cancel: refund=%d\n", credit);
                credit = 0;
                state = WAITING;
                break;
            default:
                printf("ignored: %c\n", event);
                break;
        }
    }
    printf("Final state=%d credit=%d drinks=%d\n", state, credit, drinks);
    return 0;
}
