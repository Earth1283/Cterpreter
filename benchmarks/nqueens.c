#include <stdio.h>
#define N 11
int col[N];
int safe(int row, int c) {
    for (int r = 0; r < row; r++) {
        int d = col[r] - c;
        if (d == 0 || d == row - r || d == r - row) return 0;
    }
    return 1;
}
int place(int row) {
    if (row == N) return 1;
    int count = 0;
    for (int c = 0; c < N; c++) {
        if (safe(row, c)) {
            col[row] = c;
            count += place(row + 1);
        }
    }
    return count;
}
int main(void) { printf("%d\n", place(0)); return 0; }
