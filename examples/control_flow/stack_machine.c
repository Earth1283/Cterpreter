#include <stdio.h>

#define STACK_CAPACITY 16

enum Opcode { HALT, PUSH, ADD, SUBTRACT, MULTIPLY, DUPLICATE, PRINT };

int run(int *program, int length) {
    int stack[STACK_CAPACITY];
    int depth = 0;
    int pc = 0;

    while (pc < length) {
        int opcode = program[pc++];
        switch (opcode) {
            case HALT:
                return 0;
            case PUSH:
                if (pc == length || depth == STACK_CAPACITY) goto invalid;
                stack[depth++] = program[pc++];
                break;
            case ADD:
            case SUBTRACT:
            case MULTIPLY:
                if (depth < 2) goto invalid;
                --depth;
                if (opcode == ADD) stack[depth - 1] += stack[depth];
                else if (opcode == SUBTRACT) stack[depth - 1] -= stack[depth];
                else stack[depth - 1] *= stack[depth];
                break;
            case DUPLICATE:
                if (depth == 0 || depth == STACK_CAPACITY) goto invalid;
                stack[depth] = stack[depth - 1];
                ++depth;
                break;
            case PRINT:
                if (depth == 0) goto invalid;
                printf("stack[%d] = %d\n", depth - 1, stack[depth - 1]);
                break;
            default:
                goto invalid;
        }
    }

invalid:
    fprintf(stderr, "Invalid instruction or stack operation at pc=%d.\n", pc);
    return 1;
}

int main(void) {
    int program[] = {
        PUSH, 7, PUSH, 5, ADD,
        PUSH, 3, MULTIPLY,
        PUSH, 4, SUBTRACT,
        DUPLICATE, MULTIPLY,
        PRINT, HALT
    };
    return run(program, sizeof(program) / sizeof(int));
}
