# Changelog

## 0.4.0 — 2026-09-25

Programs run about **2.8× faster** than 0.3.1. Output, diagnostics, and strict-mode behaviour are unchanged. The exceptions are the step and depth limits, which now count work differently (see below), and one crash that is now a diagnostic. Most of the speed comes from a new pass that runs over each submission after parsing and before anything executes.

### Benchmarks

Release builds of 0.3.1 (`24f452c`) and 0.4.0 on the same machine, best of fifteen runs. Instruction counts are from callgrind.

| Program | 0.3.1 | 0.4.0 | Speedup | Instructions |
|---|---:|---:|---:|---:|
| `playground/brainfuck.c squares` | 989 ms | 297 ms | 3.33× | 2.78× fewer (`hello`) |
| `playground/donut.c 12` | 574 ms | 219 ms | 2.62× | 2.85× fewer (2 frames) |
| `playground/life.c gun 40` | 523 ms | 161 ms | 3.25× | 3.04× fewer (6 generations) |
| Mandelbrot, 160×120 | 183 ms | 74 ms | 2.47× | 2.94× fewer |
| Recursive `fib(27)` | 128 ms | 66 ms | 1.94× | 2.17× fewer |
| `qsort` of 20,000 ints | 163 ms | 61 ms | 2.67× | 2.42× fewer |
| Bubble sort of 700 ints | 126 ms | 44 ms | 2.86× | 3.29× fewer |
| Sieve of 200,000 | 117 ms | 44 ms | 2.66× | 2.86× fewer |
| `switch` state machine | 108 ms | 35 ms | 3.09× | 2.64× fewer |
| `sprintf`/`strlen`/`ctype` loop | 145 ms | 64 ms | 2.27× | 2.33× fewer |
| **Whole set (13 programs)** | **3347 ms** | **1196 ms** | **2.80×** | **2.63× fewer** |

### Against CPython

The same thirteen programs were ported to Python, with the same algorithms, sizes, and output, which was checked byte for byte. CPython 3.14 ran them on the same machine, best of nine runs. Times include process startup: 14 ms for CPython and 1 ms for Cterpreter.

| Program | Cterpreter 0.4.0 | CPython 3.14 | Faster |
|---|---:|---:|---|
| Mandelbrot | 72 ms | 112 ms | Cterpreter, 1.57× |
| Matrix multiply | 42 ms | 64 ms | Cterpreter, 1.55× |
| `switch` state machine | 33 ms | 51 ms | Cterpreter, 1.55× |
| Linked list and struct copies | 44 ms | 66 ms | Cterpreter, 1.51× |
| Pointer hashing | 45 ms | 56 ms | Cterpreter, 1.25× |
| Bubble sort | 43 ms | 52 ms | Cterpreter, 1.21× |
| Sieve | 44 ms | 51 ms | Cterpreter, 1.15× |
| `sprintf`/`strlen`/`ctype` loop | 63 ms | 54 ms | CPython, 1.17× |
| `qsort` with a comparator | 60 ms | 45 ms | CPython, 1.34× |
| `playground/donut.c` | 216 ms | 139 ms | CPython, 1.55× |
| Recursive `fib(27)` | 66 ms | 40 ms | CPython, 1.64× |
| `playground/life.c` | 168 ms | 90 ms | CPython, 1.87× |
| `playground/brainfuck.c` | 298 ms | 143 ms | CPython, 2.08× |
| **Whole set** | **1192 ms** | **962 ms** | CPython, 1.24× |

Cterpreter wins seven of the thirteen: the loops over locals, arrays, and structures. CPython wins where calls dominate, and where a statement's work is spread across many small nodes. Every C access is checked for bounds, lifetime, initialization, and overflow, which Python mostly doesn't need to do. 0.3.1 lost all thirteen.

### The pre-execution pass

`src/optimizer.c` runs once over every parsed submission:

- **Constant folding.** Constant subexpressions become literals, including negative literals, macro arithmetic such as `WIDTH * DISTANCE * 0.375`, casts of constants, `sizeof` of a type, and `&&`/`||` whose left operand decides the result. The folding is done by the evaluator itself, so the results are exactly what run time would produce. A fold that would fail, such as `1 / 0` or an overflow, is left in place, so the diagnostic still appears when and where it did. Case labels and enumerator values are left in their original form, because their constancy is checked on that form.
- **Dead code.** `if` with a constant condition is replaced by the branch that runs, `while (0)` is removed, and `while (1)` becomes `for (;;)`. Inside functions, empty statements, typedefs, and statements after a `return`/`break`/`continue`/`goto` up to the next label are dropped, and declaration groups are flattened into their block.
- **Lexical name binding.** Every name inside a function is linked to the declaration it names, and the runtime already tracks each declaration's live symbol. Names the function never declares go straight to the globals. Functions with `goto`, or with a declaration directly in a `switch` body, keep dynamic lookup, because a jump can make the two disagree. Declarations that nothing looks up by name are no longer hash-indexed or checked for duplicates in their scope.
- **Unaddressed locals.** A scalar local or parameter whose address is never taken keeps its bytes in its symbol, so it needs no allocation record. No pointer can reach it, so no dangling-pointer diagnostic is lost.
- **Specialized evaluators.** Each expression and statement node is given a handler chosen for its shape: local reads by type, each arithmetic and comparison operator, local assignment and `++`/`--`, array indexing, member access, dereference, assignment through an index, member, or pointer, calls, and each statement kind. Every handler checks its assumptions at run time. When one fails, it takes the general path before doing anything observable, which is what keeps every diagnostic the same.
- **Scopeless blocks.** A block that declares nothing and cannot create a temporary runs in its enclosing scope.

### Evaluation

- Arithmetic on every combination of the basic integer and floating types takes an inline path through a precomputed common-type table. Only the cases that could fail go to the general code. Overflow checks use the compiler's overflow builtins where available.
- Constants and initialized locals are read in place wherever they are operands, without a dispatch.
- Array indexing and member access remember the element or member layout for the operand type they last saw, and they combine the bounds check with the load.
- Arrays filled one element at a time are recognized as fully initialized once every byte has been written. Reads of them then skip the per-byte check.
- Statements report their control flow as a small code instead of a 40-byte structure. Return values and `goto` targets are held by the interpreter while they unwind.
- The step counter needs one comparison per step. The failure, exit, limit, and interrupt checks run only at a precomputed boundary.

### Limits

- **Steps.** A step is still one statement or one evaluated node. Constants and local variables read as operands no longer cost a step, and neither do folded expressions, so a program now uses fewer steps than before and never more.
- **Depth.** The depth limit exists to bound the host stack. Statements still count one frame each. A call now counts every evaluation that encloses it in its statement, in one charge, instead of each evaluation counting as it happens, because expression nesting is already capped by the parser. For the deepest recursion `--max-depth 8192` allows, the host stack needed fell from 3–5 MB to 1.5–3.7 MB, depending on the shape of the recursion. The limit may now trip at a different point in a recursion, and a deep recursion can report "evaluation nesting limit exceeded" where it reported "execution nesting limit exceeded", or the reverse.

### Examples

- A new `examples/unhinged` folder: a quine; a Forth that compiles to threaded code; a Lisp with closures and the Y combinator; arithmetic done by reducing S, K, and I combinators; a factorial that runs its own file in nested interpreters, one level per factor; and a chase through 45 shuffled rooms that ends in a 46-star dereference. Five of them join the native reference comparison. Inception runs its computation only under Cterpreter, so it is left out.

### Fixed

- An expression that indexed or dereferenced a non-pointer inside a `?:` crashed the interpreter with a stack overflow while working out the expression's type, as in `c ? x[0] : 2` with an `int x`. It now reports "dereference requires a pointer".

### Verification

- All 9 CTest checks pass in the normal build and the AddressSanitizer/UndefinedBehaviorSanitizer build. The core tests gain cases for folding next to failing expressions, `&&` folding with a non-constant operand, shadowing under recursion, use before a later declaration, unaddressed and escaping locals, dead code around labels, pointer walks, mixed-width arithmetic, and deep recursion. Every new case also passes on 0.3.1, except the crash above.
- 209 differential cases (edge-case programs in normal and strict mode, every example, the benchmarks, and scripted REPL sessions) produce byte-identical stdout, stderr, and exit status under 0.3.1 and 0.4.0. The only allowed difference is where a step or depth limit trips.
- 6,400 randomly generated programs were run under both versions, using the final build. They mix every basic type, arrays, pointers, loops, calls, compound assignment, casts, overflow, division by zero, and out-of-bounds and uninitialized access. The results are identical apart from one program that 0.3.1 stopped at the step limit and 0.4.0 finished. Another 852 were run under the sanitizer build with no reports. Earlier rounds of this testing found one bug in the new pass, since fixed: it folded `1 && x` by evaluating `x`.

## 0.3.1 — 2026-09-23

Programs run about **2.7× faster** overall with no change to their output, diagnostics, or strict-mode behaviour, except for the order `qsort` gives equal elements, described below. The language gained the features Cterpreter's own source needs, so it can now parse and load every file of its interpreter core.

### Benchmarks

Release builds of commit `180f2e7` (the last commit before this work) and 0.3.1 on the same machine, best of five runs. Instruction counts are from callgrind.

| Program | Before | 0.3.1 | Speedup | Instructions |
|---|---:|---:|---:|---:|
| `playground/donut.c` | 6562 ms | 2372 ms | 2.77× | 2.74× fewer (3 frames) |
| `playground/life.c` | 1860 ms | 762 ms | 2.44× | 2.34× fewer (8 generations) |
| `qsort` of 20,000 ints | 758 ms | 165 ms | 4.59× | 4.55× fewer |
| Mandelbrot, 80×60 | 275 ms | 103 ms | 2.67× | |
| Recursive `fib(25)` | 138 ms | 55 ms | 2.53× | 2.51× fewer |
| `switch` state machine | 87 ms | 35 ms | 2.50× | |
| Sieve of 200,000 | 311 ms | 129 ms | 2.41× | 2.31× fewer |
| `sprintf`/`strlen`/`ctype` loop | 183 ms | 91 ms | 2.02× | 1.90× fewer |
| Linked list and struct copies | 96 ms | 55 ms | 1.76× | |
| **Whole set (15 programs)** | **10560 ms** | **3906 ms** | **2.70×** | |

### Memory

- Scalars and objects up to 16 bytes store their bytes and initialization bitmap inside their allocation record, so declaring a local no longer calls the host allocator. Records that have aged out of the dead-object history are reused.
- Object lookup checks the most recently used record, then a 256-slot cache, then binary-searches a contiguous array of addresses.
- Reads and writes through a variable use the variable's own record directly, skipping the lookup, alignment, and bounds checks that can't fail for it.

### Name resolution

- Each name remembers its last resolution. The result stays valid while the innermost scope instance and its symbol count are unchanged.
- Each scope keeps a 128-bit filter of the names it declares, so looking a name up usually costs one bit test per scope instead of a scan.
- Each local declaration records its innermost live symbol and restores the previous one when its scope ends. As a result, a cached resolution survives a block being re-entered on every loop iteration and stays correct under recursion.
- Name hashes are finalized with splitmix64, because raw FNV-1a left the high bits the filters use poorly mixed.

### Evaluation

- `int` and `double` binary operators take a fast path. Mixed-type arithmetic takes the general path.
- Converting a value to the type it already has returns immediately.
- A `switch` validates and evaluates its case labels once. Before, it checked every pair of labels on every execution.
- The type of a `?:` expression and the label a `goto` jumps to are cached.
- Tokens are passed by pointer through the arithmetic, load, and store paths instead of copying 56 bytes per call. The interrupt flag is polled every 1024 steps instead of on every step.
- Leaf nodes (literals and variables) are evaluated without entering the general evaluator.
- Integer limits are precomputed, the pointer-type cache is checked inline, and a basic type's kind is read from its handle without a table lookup.

### Library

- Builtins dispatch on an integer id instead of chains of string comparisons.
- `qsort` is now a stable bottom-up merge sort. The old shell sort called the comparator O(n^1.5) times. **Behaviour change:** elements that compare equal now keep their original order, which matches glibc. Before, they came out in an arbitrary order. The shell sort remains as a fallback if the scratch buffer would exceed the memory limit.
- `printf` and its relatives copy runs of literal text in one step and format each conversion once instead of twice.
- `strlen`, `puts`, and other string arguments use `memchr` on fully initialized objects.

### Parsing and calls

- The lexer matches operators with a switch on the first character instead of trying 22 strings on every token.
- The preprocessor emits runs of punctuation and whitespace in one step instead of byte by byte.
- Calls through function pointers, including library callbacks, find their target by binary search instead of walking a list of every function.

### Language

These close the gaps that stopped Cterpreter from parsing its own source. All seven files of the interpreter core, plus `boot.c`, now load under the interpreter. The command-line front end still needs POSIX headers such as `unistd.h` and `termios.h`.

- `signal.h` (with `sig_atomic_t` and the standard signal numbers) and `inttypes.h` (with the `PRI*` format macros) are available. `stdint.h` gained the `*_MIN` limits and the `INTn_C`/`UINTn_C` constant macros.
- `_Static_assert` works at file scope, block scope, and inside structures, and `assert.h` defines `static_assert`. The assertion is checked while parsing, so it costs nothing at run time.
- The comma operator, wherever C allows a full expression: statements, `for` clauses, conditions, `return`, parentheses, subscripts, and the middle of `?:`.
- A file-scope object may be declared more than once, as in `extern int x;` followed by `int x = 1;`, as long as the types agree and at most one declaration initializes it. A block-scope `extern` refers to the file-scope object instead of creating a new local.
- `sizeof` folds in constant expressions for objects declared earlier in the same source, so `int order[sizeof table / sizeof table[0]];` and `enum { N = sizeof buffer };` work. Objects from earlier REPL entries are still sized at run time only.
- Fixed: including a built-in header that carries declarations, such as `stdarg.h`, shifted every later line number in diagnostics.

Measured with callgrind, these additions cost at most 0.3% more instructions, on programs that declare many locals. Recursive calls are 1.5% cheaper, because the cached type of a `?:` expression is now read without a function call.

### Verification

- All 9 CTest checks pass in the normal build and the AddressSanitizer/UndefinedBehaviorSanitizer build, including new cases for the headers, `_Static_assert`, the comma operator, redeclarations, and `sizeof` folding.
- The interpreter-core files load under the sanitizer build with no reports.
- Twenty programs covering shadowing, recursion, `goto`, `switch` fallthrough, dangling and out-of-bounds pointers, uninitialized reads, overflow, `printf` formats, varargs, function pointers, and strict mode were run under both the previous commit and 0.3.1, along with every example and a scripted REPL session. Output, errors, and exit status are identical apart from the `qsort` tie order.

### Also since 0.3.0

This release also includes the work logged in [PROGRESS.md](PROGRESS.md) since 0.3.0: variadic functions and `stdarg.h`, the `v*printf`/`v*scanf` family, and the `.config` CLI preferences.
