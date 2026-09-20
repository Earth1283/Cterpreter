#include <stdio.h>
#include <stdlib.h>

int columns[7];
int board_size = 6;
int solutions = 0;

int can_place(int row, int column) {
    for (int previous = 0; previous < row; ++previous) {
        int distance = abs(columns[previous] - column);
        if (distance == 0 || distance == row - previous) return 0;
    }
    return 1;
}

void place_queens(int row) {
    if (row == board_size) {
        ++solutions;
        if (solutions == 1) {
            printf("First placement:");
            for (int i = 0; i < board_size; ++i) printf(" %d", columns[i]);
            putchar('\n');
        }
        return;
    }

    for (int column = 0; column < board_size; ++column) {
        if (!can_place(row, column)) continue;
        columns[row] = column;
        place_queens(row + 1);
    }
}

int main(int argc, char **argv) {
    if (argc > 1) board_size = atoi(argv[1]);
    if (board_size < 1 || board_size > 7) {
        fprintf(stderr, "Board size must be between 1 and 7.\n");
        return 2;
    }
    place_queens(0);
    printf("%d queens: %d solutions\n", board_size, solutions);
    return 0;
}
