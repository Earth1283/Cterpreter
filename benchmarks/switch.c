#include <stdio.h>
int main(void) {
    int state = 0, sum = 0;
    for (int i = 0; i < 400000; i++) {
        switch (state) {
            case 0: sum += 1; break;
            case 1: sum += 3; break;
            case 2: sum += 5; break;
            case 3: sum += 7; break;
            case 4: sum += 11; break;
            case 5: sum += 13; break;
            case 6: sum += 17; break;
            case 7: sum += 19; break;
            case 8: sum += 23; break;
            case 9: sum += 29; break;
            case 10: sum += 31; break;
            case 11: sum += 37; break;
            case 12: sum += 41; break;
            case 13: sum += 43; break;
            case 14: sum += 47; break;
            default: sum += 53; break;
        }
        state = (state + 7) & 15;
    }
    printf("%d\n", sum);
    return 0;
}
