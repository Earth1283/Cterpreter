#!/usr/bin/env python3
"""Compare stdout, stderr and status of two builds on seeded runtime kernels.

Usage: python3 tests/differential.py BEFORE AFTER [--count 1200]
"""
import argparse
import random
import resource
import subprocess


def cases(rng, count):
    types = ("int", "unsigned", "long", "unsigned long", "char", "short", "double")
    ops = ("+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "<", ">", "<=", ">=", "==", "!=")
    for index in range(count):
        a, b = rng.randint(-50000, 50000), rng.randint(-5, 32)
        op = rng.choice(ops)
        kind = index % 7
        if kind == 0:
            left, right = rng.choice(types), rng.choice(types)
            # Includes invalid operands, mixed conversions, and signed overflow.
            yield f"int f({left} a, {right} b) {{ return (int)((a {op} b) + (a & 255)); }} f({a}, {b})"
        elif kind == 1:
            update = rng.choice(("=", "+=", "-=", "*="))
            initial = rng.choice((str(a), "2147483647", "(-2147483647 - 1)"))
            rhs = rng.choice((str(b), f"{b}.5", "(x = 5)", "(x++ + 1)"))
            yield f"int f(void) {{ int x = {initial}; x {update} {rhs}; return x; }} f()"
        elif kind == 2:
            stop = rng.randint(0, 12)
            relation = rng.choice(("<", "<=", "!=", ">", ">=", "=="))
            initialized = rng.choice((" = 0", ""))
            yield (f"int f(void) {{ int x{initialized}, sum = 0; for (int i = 0; i < {stop}; i++) {{ "
                   f"if (i {relation} 3) sum += i; else sum -= 2; }} "
                   f"while (x < {stop}) x++; do {{ sum++; }} while (sum < 0); return x + sum; }} f()")
        elif kind == 3:
            position = rng.randint(-1, 4)
            lifetime = rng.choice(("", "free(p);"))
            yield (f"int f(void) {{ int *p = calloc(4, sizeof(int)); p[0] = {a}; p[1] = {b}; "
                   f"{lifetime} return (p[{position}] + 2) * (p[1] - 3); }} f()")
        elif kind == 4:
            # The same builtin node runs repeatedly, including under a pointer
            # shadow and with argument side effects.
            yield (f"int custom(int n) {{ return n + 100; }} int f(int n) {{ int sum = abs(n); "
                   f"int (*abs)(int) = custom; sum += abs(n); return sum; }} f({b}) + f({b + 1})")
        elif kind == 5:
            # Aggregate arguments must keep caller temporaries alive even
            # when the directly called function returns a scalar.
            yield ("struct S { int x, y; }; struct S *saved; "
                   "int keep(struct S *p) { saved = p; return p->x; } "
                   f"int f(void) {{ int x = keep(&(struct S){{{a}, {b}}}); return x + saved->y; }} "
                   "f(); saved->x")
        else:
            first = rng.choice((str(a) + ".5", "1e308", "1e-300", "0.0"))
            second = rng.choice((str(b) + ".5", "1e308", "0.0", "1.0"))
            yield (f"double f(double a, double b) {{ return ((a * 2.0 + b) / (b - 1.0)); }} "
                   f"(int)(f({first}, {second}) < 10.0)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--count", type=int, default=1200)
    parser.add_argument("--seed", type=int, default=20261004)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    rng = random.Random(args.seed)
    for index, source in enumerate(cases(rng, args.count)):
        options = ["--no-config", "--no-history", "--max-steps", "100000"]
        if index % 7 == 0:
            options += ["--max-steps", str(rng.randint(1, 40))]
        if index % 11 == 0:
            options += ["--max-depth", str(rng.randint(1, 15))]
        if index % 3 == 0:
            options += ["--strict"]
        results = []
        for binary in (args.before, args.after):
            run = subprocess.run([binary, *options, "-e", source], capture_output=True, timeout=5)
            results.append((run.returncode, run.stdout, run.stderr))
        if results[0] != results[1]:
            raise AssertionError(f"case {index}, seed {args.seed}, options {options}\n{source}\n"
                                 f"before={results[0]!r}\nafter={results[1]!r}")
    print(f"{args.count} differential cases passed (seed {args.seed}); status, stdout and stderr identical")


if __name__ == "__main__":
    main()
