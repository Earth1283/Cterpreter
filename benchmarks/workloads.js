"use strict";
// JavaScript ports: same loops, sizes and checksums as the adjacent C files.
// Numeric arrays are typed arrays; 32-bit unsigned arithmetic uses Math.imul and >>> 0.

function fib(n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

function mix(x, y) {
    return (x * 3 + y) % 1009;
}

function calls() {
    let total = 0;
    for (let i = 0; i < 500000; i++) total += mix(i % 997, i % 31);
    console.log(total);
}

function integers() {
    let x = 7, total = 0;
    for (let i = 0; i < 1000000; i++) {
        x = (x * 17 + 23) % 1009;
        total += (x & 255) ^ (i & 127);
    }
    console.log(x, total);
}

function sieveOf(n) {
    const flags = new Uint8Array(n);
    let count = 0;
    for (let i = 2; i * i < n; i++) {
        if (!flags[i]) for (let j = i * i; j < n; j += i) flags[j] = 1;
    }
    for (let i = 2; i < n; i++) if (!flags[i]) count++;
    console.log(count);
}

function bubble() {
    const a = new Int32Array(900);
    for (let i = 0; i < 900; i++) a[i] = (i * 7919 + 123) % 1009;
    for (let i = 0; i < 899; i++) {
        for (let j = 0; j < 899 - i; j++) {
            if (a[j] > a[j + 1]) {
                const t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
    let total = 0;
    for (let i = 0; i < 900; i++) total += (i % 31) * a[i];
    console.log(total);
}

function matrixOf(n) {
    const a = new Int32Array(n * n), b = new Int32Array(n * n), c = new Int32Array(n * n);
    for (let i = 0; i < n * n; i++) {
        a[i] = i % 17;
        b[i] = i % 13;
    }
    for (let i = 0; i < n; i++)
        for (let j = 0; j < n; j++)
            for (let k = 0; k < n; k++) c[i * n + j] += a[i * n + k] * b[k * n + j];
    let total = 0;
    for (let i = 0; i < n * n; i++) total += c[i];
    console.log(total);
}

function mandelbrotOf(width, height, limit) {
    let total = 0;
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
            const cr = x * 3.0 / width - 2.0, ci = y * 2.0 / height - 1.0;
            let zr = 0.0, zi = 0.0, n = 0;
            while (zr * zr + zi * zi < 4.0 && n < limit) {
                const next = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = next;
                n++;
            }
            total += n;
        }
    }
    console.log(total);
}

function switchCase() {
    let state = 0, total = 0;
    for (let i = 0; i < 400000; i++) {
        switch (state) {
            case 0: total += 1; break;
            case 1: total += 3; break;
            case 2: total += 5; break;
            case 3: total += 7; break;
            case 4: total += 11; break;
            case 5: total += 13; break;
            case 6: total += 17; break;
            case 7: total += 19; break;
            case 8: total += 23; break;
            case 9: total += 29; break;
            case 10: total += 31; break;
            case 11: total += 37; break;
            case 12: total += 41; break;
            case 13: total += 43; break;
            case 14: total += 47; break;
            default: total += 53; break;
        }
        state = (state + 7) & 15;
    }
    console.log(total);
}

function stringsOf(count) {
    let total = 0;
    for (let i = 0; i < count; i++) {
        const text = "item-" + String(i).padStart(6, "0");
        total += text.length;
        for (let j = 0; j < text.length; j++) {
            const code = text.charCodeAt(j);
            if (code >= 48 && code <= 57) total++;
        }
    }
    console.log(total);
}

function nqueens() {
    const n = 11, col = new Int32Array(n);
    function safe(row, c) {
        for (let r = 0; r < row; r++) {
            const d = col[r] - c;
            if (d === 0 || d === row - r || d === r - row) return 0;
        }
        return 1;
    }
    function place(row) {
        if (row === n) return 1;
        let count = 0;
        for (let c = 0; c < n; c++) {
            if (safe(row, c)) {
                col[row] = c;
                count += place(row + 1);
            }
        }
        return count;
    }
    console.log(place(0));
}

function collatz() {
    let best = 0, arg = 0, total = 0;
    for (let n = 1; n < 150000; n++) {
        let x = n, steps = 0;
        while (x !== 1) {
            if (x % 2 === 0) x = x / 2;
            else x = 3 * x + 1;
            steps++;
        }
        total += steps;
        if (steps > best) {
            best = steps;
            arg = n;
        }
    }
    console.log(arg, best, total);
}

function lcg(s) {
    return (Math.imul(s, 1103515245) + 12345) >>> 0;
}

function quicksort() {
    function sort(a, lo, hi) {
        while (lo < hi) {
            const pivot = a[(lo + hi) >> 1];
            let i = lo, j = hi;
            while (i <= j) {
                while (a[i] < pivot) i++;
                while (a[j] > pivot) j--;
                if (i <= j) {
                    const t = a[i];
                    a[i] = a[j];
                    a[j] = t;
                    i++;
                    j--;
                }
            }
            if (j - lo < hi - i) {
                sort(a, lo, j);
                lo = i;
            } else {
                sort(a, i, hi);
                hi = j;
            }
        }
    }
    const n = 200000, a = new Int32Array(n);
    let s = 12345;
    for (let i = 0; i < n; i++) {
        s = lcg(s);
        a[i] = (s >>> 8) & 0xFFFFF;
    }
    sort(a, 0, n - 1);
    let total = 0;
    for (let i = 0; i < n; i++) {
        if (i > 0 && a[i - 1] > a[i]) {
            console.log("unsorted");
            process.exit(1);
        }
        total += (i % 101) * a[i];
    }
    console.log(total);
}

function lcs() {
    const n = 2000, x = new Uint8Array(n), y = new Uint8Array(n);
    let s = 99;
    for (let i = 0; i < n; i++) {
        s = lcg(s);
        x[i] = (s >>> 16) & 3;
        s = lcg(s);
        y[i] = (s >>> 16) & 3;
    }
    let prev = new Int32Array(n + 1), cur = new Int32Array(n + 1);
    for (let i = 1; i <= n; i++) {
        for (let j = 1; j <= n; j++) {
            if (x[i - 1] === y[j - 1]) cur[j] = prev[j - 1] + 1;
            else cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
        }
        const t = prev;
        prev = cur;
        cur = t;
    }
    console.log(prev[n]);
}

function spectral() {
    const n = 200;
    const A = (i, j) => 1.0 / (((i + j) * (i + j + 1) / 2 | 0) + i + 1);
    function mulAv(v, out) {
        for (let i = 0; i < n; i++) {
            let sum = 0.0;
            for (let j = 0; j < n; j++) sum += A(i, j) * v[j];
            out[i] = sum;
        }
    }
    function mulAtv(v, out) {
        for (let i = 0; i < n; i++) {
            let sum = 0.0;
            for (let j = 0; j < n; j++) sum += A(j, i) * v[j];
            out[i] = sum;
        }
    }
    const u = new Float64Array(n).fill(1.0), v = new Float64Array(n), t = new Float64Array(n);
    for (let k = 0; k < 10; k++) {
        mulAv(u, t);
        mulAtv(t, v);
        mulAv(v, t);
        mulAtv(t, u);
    }
    let vbv = 0.0, vv = 0.0;
    for (let i = 0; i < n; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    console.log(Math.sqrt(vbv / vv).toFixed(9));
}

function hash() {
    let h = 2166136261, s = 1;
    for (let i = 0; i < 3000000; i++) {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        h = (h ^ ((s >>> 24) & 255)) >>> 0;
        h = Math.imul(h, 16777619) >>> 0;
        h = (h ^ (h >>> 13)) >>> 0;
        h = (h + (i & 1023)) >>> 0;
    }
    console.log(h, s);
}

const workloads = {
    fib: () => console.log(fib(29)),
    calls,
    integers,
    sieve: () => sieveOf(250000),
    bubble,
    matrix: () => matrixOf(48),
    mandelbrot: () => mandelbrotOf(160, 120, 80),
    switch: switchCase,
    strings: () => stringsOf(40000),
    fib_long: () => console.log(fib(35)),
    sieve_long: () => sieveOf(8000000),
    nqueens,
    collatz,
    quicksort,
    matrix_long: () => matrixOf(120),
    lcs,
    mandelbrot_long: () => mandelbrotOf(320, 240, 200),
    spectral,
    strings_long: () => stringsOf(300000),
    hash,
};

workloads[process.argv[2]]();
