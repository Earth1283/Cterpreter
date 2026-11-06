#include <stdio.h>
#include <math.h>
#define N 200
double A(int i, int j) { return 1.0 / ((i + j) * (i + j + 1) / 2 + i + 1); }
void mul_av(double *v, double *out) {
    for (int i = 0; i < N; i++) {
        double s = 0.0;
        for (int j = 0; j < N; j++) s += A(i, j) * v[j];
        out[i] = s;
    }
}
void mul_atv(double *v, double *out) {
    for (int i = 0; i < N; i++) {
        double s = 0.0;
        for (int j = 0; j < N; j++) s += A(j, i) * v[j];
        out[i] = s;
    }
}
int main(void) {
    double u[N], v[N], t[N];
    for (int i = 0; i < N; i++) u[i] = 1.0;
    for (int k = 0; k < 10; k++) {
        mul_av(u, t); mul_atv(t, v);
        mul_av(v, t); mul_atv(t, u);
    }
    double vbv = 0.0, vv = 0.0;
    for (int i = 0; i < N; i++) { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
    printf("%.9f\n", sqrt(vbv / vv));
    return 0;
}
