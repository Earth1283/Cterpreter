#include <stdio.h>

int main(void) {
    puts("Enter integer calculations, one per line. EOF finishes.");
    int left;
    int right;
    char operator;

    while (1) {
        int fields = scanf("%d %c %d", &left, &operator, &right);
        if (fields == EOF) return 0;
        if (fields != 3) {
            fprintf(stderr, "Expected: integer operator integer.\n");
            return 1;
        }
        int result = 0;
        switch (operator) {
            case '+': result = left + right; break;
            case '-': result = left - right; break;
            case '*': result = left * right; break;
            case '/':
                if (right == 0) {
                    puts("Cannot divide by zero.");
                    continue;
                }
                result = left / right;
                break;
            default:
                printf("Unknown operator: %c\n", operator);
                continue;
        }
        printf("%d %c %d = %d\n", left, operator, right, result);
    }
}
