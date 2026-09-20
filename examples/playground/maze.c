#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COLUMNS 79
#define MAX_ROWS 39
#define AT(x, y) grid[(y) * columns + (x)]

static char grid[MAX_COLUMNS * MAX_ROWS];
static int columns = 31, rows = 15;
static const int step_x[] = {0, 0, -2, 2};
static const int step_y[] = {-2, 2, 0, 0};

static void carve(int x, int y) {
    AT(x, y) = ' ';
    int order[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; i--) {
        int j = rand() % (i + 1);
        int swap = order[i];
        order[i] = order[j];
        order[j] = swap;
    }
    for (int i = 0; i < 4; i++) {
        int direction = order[i];
        int nx = x + step_x[direction], ny = y + step_y[direction];
        if (nx <= 0 || ny <= 0 || nx >= columns - 1 || ny >= rows - 1) continue;
        if (AT(nx, ny) == ' ') continue;
        AT(x + step_x[direction] / 2, y + step_y[direction] / 2) = ' ';
        carve(nx, ny);
    }
}

static int solve(int x, int y, int goal_x, int goal_y) {
    if (AT(x, y) != ' ') return 0;
    AT(x, y) = '.';
    if (x == goal_x && y == goal_y) return 1;
    for (int direction = 0; direction < 4; direction++)
        if (solve(x + step_x[direction] / 2, y + step_y[direction] / 2, goal_x, goal_y)) return 1;
    AT(x, y) = ' ';
    return 0;
}

static void show(const char *title) {
    char line[MAX_COLUMNS + 1];
    printf("%s\n", title);
    for (int y = 0; y < rows; y++) {
        memcpy(line, grid + y * columns, (size_t)columns);
        line[columns] = '\0';
        puts(line);
    }
}

static int odd_within(const char *text, int fallback, int limit) {
    int value = text ? atoi(text) : fallback;
    if (value < 7) value = 7;
    if (value > limit) value = limit;
    return value | 1;
}

int main(int argc, char **argv) {
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 1u;
    columns = odd_within(argc > 2 ? argv[2] : NULL, columns, MAX_COLUMNS);
    rows = odd_within(argc > 3 ? argv[3] : NULL, rows, MAX_ROWS);

    srand(seed);
    memset(grid, '#', sizeof grid);
    carve(1, 1);
    AT(columns - 2, rows - 2) = ' ';
    show("carved");

    if (!solve(1, 1, columns - 2, rows - 2)) { printf("\nno route with seed %u\n", seed); return 1; }
    int length = 0;
    for (int i = 0; i < columns * rows; i++) length += grid[i] == '.';
    printf("\nseed %u, %dx%d, route of %d cells\n", seed, columns, rows, length);
    show("solution");
    return 0;
}
