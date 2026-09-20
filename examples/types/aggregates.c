#include <stdio.h>
#include <string.h>

#define CREW_SIZE 3

struct Point { int x, y; };

struct Segment { struct Point start, end; };

union Reading {
    int raw;
    char bytes[4];
};

struct Member {
    char name[8];
    struct Point station;
};

static struct Point translate(struct Point point, int dx, int dy) {
    point.x += dx;
    point.y += dy;
    return point;
}

static int length_squared(const struct Segment *segment) {
    int dx = segment->end.x - segment->start.x;
    int dy = segment->end.y - segment->start.y;
    return dx * dx + dy * dy;
}

int main(void) {
    struct Point origin = {0, 0};
    struct Point shifted = translate(origin, 3, 4);
    printf("origin (%d, %d) shifted (%d, %d)\n", origin.x, origin.y, shifted.x, shifted.y);

    struct Segment diagonal = { {1, 1}, {4, 5} };
    printf("length squared %d\n", length_squared(&diagonal));

    struct Segment copy = diagonal;
    copy.end = (struct Point){10, 10};
    printf("copy end (%d, %d), original end (%d, %d)\n",
           copy.end.x, copy.end.y, diagonal.end.x, diagonal.end.y);

    struct Member crew[CREW_SIZE] = {
        { .name = "ada", .station = { .x = 1, .y = 2 } },
        { "grace", {3, 4} },
        { .station = {5, 6}, .name = "alan" }
    };
    for (int i = 0; i < CREW_SIZE; i++)
        printf("%-6s at (%d, %d)\n", crew[i].name, crew[i].station.x, crew[i].station.y);

    int grid[3][4] = { {1, 2, 3, 4}, {5, 6, 7, 8} };
    grid[2][0] = 9;
    int total = 0;
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 4; column++) total += grid[row][column];
    printf("grid total %d, row bytes %d, grid bytes %d\n",
           total, (int)sizeof(grid[0]), (int)sizeof(grid));

    union Reading reading;
    memset(&reading, 0, sizeof reading);
    reading.bytes[0] = 'A';
    printf("union holds %d in %d bytes\n", reading.raw, (int)sizeof(union Reading));

    struct Point sparse[4] = { [3] = {7, 8} };
    printf("sparse first (%d, %d) last (%d, %d)\n",
           sparse[0].x, sparse[0].y, sparse[3].x, sparse[3].y);
    return 0;
}
