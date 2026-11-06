"""Python ports: same loops, sizes and checksums as the adjacent C files."""
import math
import sys


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def mix(x, y):
    return (x * 3 + y) % 1009


def calls():
    total = 0
    for i in range(500000):
        total += mix(i % 997, i % 31)
    print(total)


def integers():
    x, total = 7, 0
    for i in range(1000000):
        x = (x * 17 + 23) % 1009
        total += (x & 255) ^ (i & 127)
    print(x, total)


def sieve():
    n, count = 250000, 0
    flags = bytearray(n)
    i = 2
    while i * i < n:
        if not flags[i]:
            for j in range(i * i, n, i):
                flags[j] = 1
        i += 1
    for i in range(2, n):
        if not flags[i]:
            count += 1
    print(count)


def bubble():
    a = [(i * 7919 + 123) % 1009 for i in range(900)]
    for i in range(899):
        for j in range(899 - i):
            if a[j] > a[j + 1]:
                t = a[j]
                a[j] = a[j + 1]
                a[j + 1] = t
    total = 0
    for i in range(900):
        total += (i % 31) * a[i]
    print(total)


def matrix():
    a, b, c = [0] * (48 * 48), [0] * (48 * 48), [0] * (48 * 48)
    for i in range(48 * 48):
        a[i], b[i], c[i] = i % 17, i % 13, 0
    for i in range(48):
        for j in range(48):
            for k in range(48):
                c[i * 48 + j] += a[i * 48 + k] * b[k * 48 + j]
    total = 0
    for i in range(48 * 48):
        total += c[i]
    print(total)


def mandelbrot():
    total = 0
    for y in range(120):
        for x in range(160):
            cr, ci = x * 3.0 / 160 - 2.0, y * 2.0 / 120 - 1.0
            zr, zi, n = 0.0, 0.0, 0
            while zr * zr + zi * zi < 4.0 and n < 80:
                nxt = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = nxt
                n += 1
            total += n
    print(total)


def switch():
    state, total = 0, 0
    for i in range(400000):
        match state:
            case 0: total += 1
            case 1: total += 3
            case 2: total += 5
            case 3: total += 7
            case 4: total += 11
            case 5: total += 13
            case 6: total += 17
            case 7: total += 19
            case 8: total += 23
            case 9: total += 29
            case 10: total += 31
            case 11: total += 37
            case 12: total += 41
            case 13: total += 43
            case 14: total += 47
            case _: total += 53
        state = (state + 7) & 15
    print(total)


def strings():
    total = 0
    for i in range(40000):
        text = "item-%06d" % i
        total += len(text)
        for j in range(len(text)):
            if text[j].isdigit():
                total += 1
    print(total)


def fib_long():
    print(fib(35))


def sieve_long():
    n, count = 8000000, 0
    flags = bytearray(n)
    i = 2
    while i * i < n:
        if not flags[i]:
            for j in range(i * i, n, i):
                flags[j] = 1
        i += 1
    for i in range(2, n):
        if not flags[i]:
            count += 1
    print(count)


def nqueens():
    n = 11
    col = [0] * n

    def safe(row, c):
        for r in range(row):
            d = col[r] - c
            if d == 0 or d == row - r or d == r - row:
                return 0
        return 1

    def place(row):
        if row == n:
            return 1
        count = 0
        for c in range(n):
            if safe(row, c):
                col[row] = c
                count += place(row + 1)
        return count

    print(place(0))


def collatz():
    best = arg = total = 0
    for n in range(1, 150000):
        x, steps = n, 0
        while x != 1:
            if x % 2 == 0:
                x = x // 2
            else:
                x = 3 * x + 1
            steps += 1
        total += steps
        if steps > best:
            best, arg = steps, n
    print(arg, best, total)


def quicksort():
    def sort(a, lo, hi):
        while lo < hi:
            pivot, i, j = a[(lo + hi) // 2], lo, hi
            while i <= j:
                while a[i] < pivot:
                    i += 1
                while a[j] > pivot:
                    j -= 1
                if i <= j:
                    a[i], a[j] = a[j], a[i]
                    i += 1
                    j -= 1
            if j - lo < hi - i:
                sort(a, lo, j)
                lo = i
            else:
                sort(a, i, hi)
                hi = j

    n, s = 200000, 12345
    a = [0] * n
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        a[i] = (s >> 8) & 0xFFFFF
    sort(a, 0, n - 1)
    total = 0
    for i in range(n):
        if i > 0 and a[i - 1] > a[i]:
            print("unsorted")
            sys.exit(1)
        total += (i % 101) * a[i]
    print(total)


def matrix_long():
    n = 120
    a, b, c = [0] * (n * n), [0] * (n * n), [0] * (n * n)
    for i in range(n * n):
        a[i], b[i] = i % 17, i % 13
    for i in range(n):
        for j in range(n):
            for k in range(n):
                c[i * n + j] += a[i * n + k] * b[k * n + j]
    print(sum(c))


def lcs():
    n, s = 2000, 99
    x, y = [0] * n, [0] * n
    for i in range(n):
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        x[i] = (s >> 16) & 3
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        y[i] = (s >> 16) & 3
    prev, cur = [0] * (n + 1), [0] * (n + 1)
    for i in range(1, n + 1):
        for j in range(1, n + 1):
            if x[i - 1] == y[j - 1]:
                cur[j] = prev[j - 1] + 1
            else:
                cur[j] = prev[j] if prev[j] > cur[j - 1] else cur[j - 1]
        prev, cur = cur, prev
    print(prev[n])


def mandelbrot_long():
    total = 0
    for y in range(240):
        for x in range(320):
            cr, ci = x * 3.0 / 320 - 2.0, y * 2.0 / 240 - 1.0
            zr, zi, n = 0.0, 0.0, 0
            while zr * zr + zi * zi < 4.0 and n < 200:
                nxt = zr * zr - zi * zi + cr
                zi = 2.0 * zr * zi + ci
                zr = nxt
                n += 1
            total += n
    print(total)


def spectral():
    n = 200

    def A(i, j):
        return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)

    def mul_av(v, out):
        for i in range(n):
            s = 0.0
            for j in range(n):
                s += A(i, j) * v[j]
            out[i] = s

    def mul_atv(v, out):
        for i in range(n):
            s = 0.0
            for j in range(n):
                s += A(j, i) * v[j]
            out[i] = s

    u, v, t = [1.0] * n, [0.0] * n, [0.0] * n
    for _ in range(10):
        mul_av(u, t)
        mul_atv(t, v)
        mul_av(v, t)
        mul_atv(t, u)
    vbv = vv = 0.0
    for i in range(n):
        vbv += u[i] * v[i]
        vv += v[i] * v[i]
    print("%.9f" % math.sqrt(vbv / vv))


def strings_long():
    total = 0
    for i in range(300000):
        text = "item-%06d" % i
        total += len(text)
        for j in range(len(text)):
            if text[j].isdigit():
                total += 1
    print(total)


def hash():
    h, s = 2166136261, 1
    for i in range(3000000):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        h ^= (s >> 24) & 255
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= h >> 13
        h = (h + (i & 1023)) & 0xFFFFFFFF
    print(h, s)


if __name__ == "__main__":
    name = sys.argv[1]
    if name == "fib":
        print(fib(29))
    else:
        globals()[name]()
