#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WIDTH 58
#define HEIGHT 22
#define CELLS (WIDTH * HEIGHT)
#define TORUS 2.0
#define TUBE 1.0
#define DISTANCE 5.0

static const char shades[] = ".,-~:;=!*#$@";

static void spin(double a, double b) {
    char screen[CELLS];
    double depth[CELLS];
    memset(screen, ' ', CELLS);
    for (int i = 0; i < CELLS; i++) depth[i] = 0.0;

    double sa = sin(a), ca = cos(a), sb = sin(b), cb = cos(b);
    double scale = WIDTH * DISTANCE * 0.375 / (TORUS + TUBE);

    for (double theta = 0.0; theta < 6.28; theta += 0.08) {
        double ct = cos(theta), st = sin(theta);
        double ring = TORUS + TUBE * ct, lift = TUBE * st;
        for (double phi = 0.0; phi < 6.28; phi += 0.03) {
            double cp = cos(phi), sp = sin(phi);
            double z = DISTANCE + ca * ring * sp + lift * sa;
            double near = 1.0 / z;
            double x = ring * (cb * cp + sa * sb * sp) - lift * ca * sb;
            double y = ring * (sb * cp - sa * cb * sp) + lift * ca * cb;
            int column = (int)(WIDTH / 2 + scale * near * x);
            int row = (int)(HEIGHT / 2 - scale * near * y * 0.5);
            double light = cp * ct * sb - ca * ct * sp - sa * st + cb * (ca * st - ct * sa * sp);
            if (light <= 0.0 || column < 0 || column >= WIDTH || row < 0 || row >= HEIGHT) continue;
            int cell = row * WIDTH + column;
            if (near <= depth[cell]) continue;
            depth[cell] = near;
            int level = (int)(light * 8.0);
            screen[cell] = shades[level > 11 ? 11 : level];
        }
    }

    for (int row = 0; row < HEIGHT; row++) {
        fwrite(screen + row * WIDTH, 1, WIDTH, stdout);
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 48;
    if (frames < 1) frames = 1;
    if (frames > 2000) frames = 2000;
    for (int frame = 0; frame < frames; frame++) {
        if (frame) printf("\033[%dA", HEIGHT);
        spin(frame * 0.30, frame * 0.11);
        fflush(stdout);
    }
    return 0;
}
