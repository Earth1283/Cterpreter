# Changelog

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
