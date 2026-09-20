#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 64
#define HEIGHT 24
#define CELLS (WIDTH * HEIGHT)

static const int gun[] = {
    0,4, 0,5, 1,4, 1,5,
    10,4, 10,5, 10,6, 11,3, 11,7, 12,2, 12,8, 13,2, 13,8, 14,5,
    15,3, 15,7, 16,4, 16,5, 16,6, 17,5,
    20,2, 20,3, 20,4, 21,2, 21,3, 21,4, 22,1, 22,5,
    24,0, 24,1, 24,5, 24,6, 34,2, 34,3, 35,2, 35,3
};
static const int acorn[] = { 1,0, 3,1, 0,2, 1,2, 4,2, 5,2, 6,2 };
static const int pentomino[] = { 1,0, 2,0, 0,1, 1,1, 1,2 };
static const int diehard[] = { 6,0, 0,1, 1,1, 1,2, 5,2, 6,2, 7,2 };

static void seed(char *grid, const int *cells, int count, int left, int top) {
    for (int i = 0; i < count; i++) {
        int column = (left + cells[i * 2]) % WIDTH;
        int row = (top + cells[i * 2 + 1]) % HEIGHT;
        grid[row * WIDTH + column] = 1;
    }
}

static int neighbours(const char *grid, int column, int row) {
    int total = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int x = (column + dx + WIDTH) % WIDTH;
            int y = (row + dy + HEIGHT) % HEIGHT;
            total += grid[y * WIDTH + x];
        }
    return total;
}

static int step(char *grid, char *next) {
    int living = 0;
    for (int row = 0; row < HEIGHT; row++)
        for (int column = 0; column < WIDTH; column++) {
            int here = grid[row * WIDTH + column];
            int around = neighbours(grid, column, row);
            int alive = around == 3 || (here && around == 2);
            next[row * WIDTH + column] = (char)alive;
            living += alive;
        }
    memcpy(grid, next, CELLS);
    return living;
}

static void draw(const char *grid, int generation, int living) {
    char line[WIDTH + 1];
    line[WIDTH] = '\0';
    for (int row = 0; row < HEIGHT; row++) {
        for (int column = 0; column < WIDTH; column++)
            line[column] = grid[row * WIDTH + column] ? '#' : ' ';
        puts(line);
    }
    printf("generation %-5d population %-5d\n", generation, living);
}

int main(int argc, char **argv) {
    const char *pattern = argc > 1 ? argv[1] : "gun";
    int generations = argc > 2 ? atoi(argv[2]) : 60;
    if (generations < 1) generations = 1;

    char grid[CELLS], next[CELLS];
    memset(grid, 0, CELLS);

    if (!strcmp(pattern, "acorn")) seed(grid, acorn, 7, WIDTH / 2 - 3, HEIGHT / 2 - 1);
    else if (!strcmp(pattern, "r")) seed(grid, pentomino, 5, WIDTH / 2 - 1, HEIGHT / 2 - 1);
    else if (!strcmp(pattern, "diehard")) seed(grid, diehard, 7, WIDTH / 2 - 4, HEIGHT / 2 - 1);
    else if (!strcmp(pattern, "soup")) {
        srand(argc > 3 ? (unsigned)atoi(argv[3]) : 7u);
        for (int i = 0; i < CELLS; i++) grid[i] = (char)(rand() % 4 == 0);
    } else seed(grid, gun, 36, 2, 2);

    int living = 0;
    for (int i = 0; i < CELLS; i++) living += grid[i];
    draw(grid, 0, living);
    for (int generation = 1; generation <= generations; generation++) {
        living = step(grid, next);
        printf("\033[%dA", HEIGHT + 1);
        draw(grid, generation, living);
        fflush(stdout);
        if (!living) { printf("everything died\n"); break; }
    }
    return 0;
}
