#!/usr/bin/env python3
"""Run generated programs interpreted and compiled to native code; compare everything.

Usage: python3 tests/jit_differential.py CTERPRETER [--count 1500] [--seed N]

Each program mixes integer widths and signedness, doubles, conversions, shifts,
division, arrays, pointers, loops, switch, recursion and globals, so that it
often ends in a diagnostic. Stdout, stderr and the exit status must match
exactly. Limits are generous because native code charges steps per iteration.
"""
import argparse
import os
import random
import resource
import subprocess

TYPES = ("int", "unsigned", "long", "unsigned long", "char", "unsigned char", "short", "double", "long long")
INTEGERS = tuple(t for t in TYPES if t != "double")
EDGES = ("0", "1", "-1", "2", "7", "-3", "31", "32", "63", "64", "100", "255", "256", "65535",
         "2147483647", "(-2147483647 - 1)", "4294967295u", "9223372036854775807L", "1e9", "0.5", "-2.5", "1e300")


class Program:
    def __init__(self, rng):
        self.rng = rng
        self.scalars = {}

    def leaf(self):
        """A random operand and whether it is a double."""
        rng = self.rng
        roll = rng.random()
        if roll < 0.35 and self.scalars:
            name = rng.choice(sorted(self.scalars))
            return name, self.scalars[name] == "double"
        if roll < 0.5:
            return f"g[{self.index()}]", False
        if roll < 0.6:
            return f"p[{self.index()}]", False
        if roll < 0.65:
            return "gd", True
        if roll < 0.7:
            return "(*q)", False
        if rng.random() < 0.15:
            edge = rng.choice(EDGES)
            return edge, "." in edge or "e" in edge
        return str(rng.randint(-40, 40)), False

    def index(self):
        rng = self.rng
        if rng.random() < 0.9:
            return str(rng.randint(0, 7))
        return rng.choice(("-1", "8", "9", "i", "(i & 7)", "(int)gu % 8"))

    def integer(self, depth):
        text, real = self.expr(depth)
        return f"(long)({text})" if real else text

    def expr(self, depth=0):
        """A random expression and whether it is a double."""
        rng = self.rng
        if depth > 2 or rng.random() < 0.3:
            return self.leaf()
        roll = rng.random()
        risky = rng.random() < 0.1
        if roll < 0.55:
            op = rng.choice(("+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "<", ">", "<=", ">=", "==",
                             "!=", "&&", "||"))
            if op in ("%", "&", "|", "^", "<<", ">>"):
                left = self.integer(depth + 1)
                right = self.integer(depth + 1)
                if op in ("<<", ">>") and not risky:
                    return f"(({left} & 1023) {op} ({right} & 15))", False
                if op == "%" and not risky:
                    return f"({left} % ({right} | 1))", False
                return f"({left} {op} {right})", False
            (left, a), (right, b) = self.expr(depth + 1), self.expr(depth + 1)
            if op == "/" and not risky:
                right = f"(({right}) + 0.25)" if b else f"(({right}) | 1)"
            if op == "*" and not risky:
                left = f"(({left}) / 4096)" if not a else left
            real = (a or b) and op in ("+", "-", "*", "/")
            return f"({left} {op} {right})", real
        if roll < 0.7:
            kind = rng.choice(TYPES)
            text, real = self.expr(depth + 1)
            if real and kind != "double" and not risky:
                text = f"({text}) / 1e12"
            return f"(({kind})({text}))", kind == "double"
        if roll < 0.78:
            op = rng.choice(("-", "~", "!", "+"))
            if op == "~":
                return f"(~{self.integer(depth + 1)})", False
            text, real = self.expr(depth + 1)
            return f"({op}{text})", real and op != "!"
        if roll < 0.86:
            condition, _ = self.expr(depth + 1)
            (first, a), (second, b) = self.expr(depth + 1), self.expr(depth + 1)
            return f"({condition} ? {first} : {second})", a or b
        if roll < 0.9:
            return f"twice({self.integer(depth + 1)} % 64)", False
        if roll < 0.93:
            routine = rng.choice(("isdigit", "isalpha", "isspace", "toupper", "tolower", "isblank", "abs", "labs"))
            argument = self.integer(depth + 1)
            if routine not in ("abs", "labs") and not risky:
                argument = f"(({argument}) & 255)"
            return f"{routine}({argument})", False
        if roll < 0.95:
            text, _ = self.expr(depth + 1)
            return f"{rng.choice(('sqrt', 'fabs'))}({text})", True
        if self.scalars:
            name = rng.choice(sorted(self.scalars))
            real = self.scalars[name] == "double"
            return (f"({name}{rng.choice(('++', '--'))})" if rng.random() < 0.5
                    else f"({rng.choice(('++', '--'))}{name})"), real
        return self.leaf()

    def target(self):
        rng = self.rng
        roll = rng.random()
        if roll < 0.6 and self.scalars:
            return rng.choice(sorted(self.scalars))
        if roll < 0.8:
            return f"g[{self.index()}]"
        if roll < 0.9:
            return f"p[{self.index()}]"
        return rng.choice(("gl", "gd", "(*q)"))

    def statement(self, depth=0):
        rng = self.rng
        roll = rng.random()
        if roll < 0.45 or depth > 1:
            op = rng.choice(("=", "=", "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="))
            target = self.target()
            if op in ("%=", "&=", "|=", "^=", "<<=", ">>=") and target not in ("gd",) and self.scalars.get(target) != "double":
                value = self.integer(1)
                if op in ("<<=", ">>="):
                    value = f"({value} & 7)"
                elif op == "%=":
                    value = f"({value} | 1)"
                return f"{target} {op} {value};"
            if op in ("%=", "&=", "|=", "^=", "<<=", ">>="):
                op = "+="
            if op == "/=":
                return f"{target} /= (({self.integer(1)}) | 1);"
            return f"{target} {op} {self.expr()[0]};"
        if roll < 0.6:
            return f"if ({self.expr()[0]}) {{ {self.statement(depth + 1)} }} else {{ {self.statement(depth + 1)} }}"
        if roll < 0.75:
            bound = rng.randint(0, 9)
            return f"for (int k = 0; k < {bound}; k++) {{ {self.statement(depth + 1)} if (k == {rng.randint(0, 9)}) break; }}"
        if roll < 0.85:
            cases = " ".join(f"case {v}: {self.statement(depth + 1)} {rng.choice(('break;', ''))}"
                             for v in rng.sample(range(-2, 9), rng.randint(1, 5)))
            return f"switch ((int)({self.integer(0)}) & 7) {{ {cases} default: total++; }}"
        if roll < 0.92:
            return f"total += {self.integer(0)};"
        return f"q = &g[{self.index()}];"

    def build(self):
        rng = self.rng
        lines = []
        names = []
        for n in range(rng.randint(2, 5)):
            kind = rng.choice(TYPES)
            name = f"v{n}"
            initializer = f" = {self.expr()[0]}" if rng.random() < 0.85 else ""
            lines.append(f"    {kind} {name}{initializer};")
            names.append((name, kind))
            self.scalars[name] = kind
        body = [self.statement() for _ in range(rng.randint(3, 8))]
        prints = " ".join(f'printf("%.6g ", (double){name});' for name, _ in names)
        lifetime = rng.choice(("", "", "", "free(p);"))
        return ("#include <stdio.h>\n#include <stdlib.h>\n#include <ctype.h>\n#include <math.h>\n"
                "int g[8]; long gl = 5; unsigned gu = 3; double gd = 1.5;\n"
                "int twice(int x) { if (x > 1000 || x < -1000) return x / 3; return twice(x * 2 + (x < 0 ? -1 : 1)) - x; }\n"
                "long run(int i, long total, int *p, int *q) {\n" + "\n".join(lines) + "\n    "
                + lifetime + "\n    " + "\n    ".join(body) + "\n    " + prints + "\n"
                "    return total + (long)g[i & 7];\n}\n"
                "int main(void) {\n    int *p = calloc(8, sizeof(int));\n    long total = 0;\n"
                f"    for (int i = 0; i < {rng.randint(1, 4)}; i++) total += run(i, total, p, &g[{rng.randint(0, 7)}]);\n"
                '    printf("%ld %d %d\\n", total, g[3], p[2]);\n    return 0;\n}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--count", type=int, default=1500)
    parser.add_argument("--seed", type=int, default=20261106)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    rng = random.Random(args.seed)
    # Strict mode raises SIGSEGV on purpose; a sanitizer must not turn that into a report.
    environment = dict(os.environ, ASAN_OPTIONS=":".join(filter(None, (os.environ.get("ASAN_OPTIONS"), "handle_segv=0"))))
    compiled = 0
    for index in range(args.count):
        source = Program(rng).build()
        options = ["--no-config", "--no-history", "--max-steps", "50000000"]
        if index % 5 == 0:
            options.append("--strict")
        results = []
        for mode in ("off", "on"):
            run = subprocess.run([args.binary, *options, "--jit", mode, "-"], input=source.encode(),
                                 capture_output=True, timeout=60, env=environment)
            results.append((run.returncode, run.stdout, run.stderr))
        if results[0] != results[1]:
            raise AssertionError(f"case {index}, seed {args.seed}, options {options}\n{source}\n"
                                 f"interpreted={results[0]!r}\nnative={results[1]!r}")
        compiled += not results[1][2]
    print(f"{args.count} programs agree interpreted and native (seed {args.seed}); {compiled} ran to completion")


if __name__ == "__main__":
    main()
