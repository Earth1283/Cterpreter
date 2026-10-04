"""Python ports: same loops, sizes and checksums as the adjacent C files."""
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


if __name__ == "__main__":
    name = sys.argv[1]
    if name == "fib":
        print(fib(29))
    else:
        globals()[name]()
