# Cterpreter

C, but interpreted. A C17 implementation of a C interpreter, with a persistent REPL and an increasingly unreasonable [roadmap](Cterpreter_Roadmap.md).

Cterpreter evaluates its own AST; it does not invoke a compiler to execute your program. It runs C programs that use the full scalar type system, structures, unions, enumerations, multidimensional arrays, function pointers, macros, and a useful subset of the standard library. Full C17 and self-interpretation remain unfinished.

Current status: [Misendeavor log](PROGRESS.md). Browse the [feature-organized examples](examples/README.md) for runnable programs and expected results.

## Build

Requires CMake 3.20+, a C17 compiler, and macOS or Linux. The debug preset also requires Ninja.

```sh
cmake --preset debug
cmake --build --preset debug
./build/Cterpreter
```

Without Ninja: `cmake -S . -B build/local && cmake --build build/local`. CLion can open the root CMake project.

## REPL

```text
c> printf("Hello, %s!\n", "world");
Hello, world!
c> struct Point { int x, y; };
c> struct Point corner = { .y = 4, .x = 3 };
c> corner
{.x = 3, .y = 4}
c> int matrix[2][3] = {1, 2, 3, 4, 5, 6};
c> matrix[1][2]
6
c> int factorial(int n) {
...     if (n < 2) return 1;
...     return n * factorial(n - 1);
... }
c> int (*call)(int) = factorial;
c> call(6)
720
c> .type &matrix[0]
int (*)[3]
```

An expression without a final semicolon prints its value. A semicolon suppresses that result, so `printf("hello\n");` prints just `hello`. Variables, functions, types, macros, and allocated objects persist between submissions.

Incomplete syntax continues at `...`. Ctrl+C cancels input or execution, and Ctrl+D exits. Syntax errors leave the existing session intact. Runtime errors preserve effects that already completed.

The terminal supports arrows, Home/End, insertion, deletion, Ctrl+A/E, Ctrl+K/U/W, history recall, and Ctrl+R to search earlier history for the current text. Tab accepts an inline suggestion drawn from the session's own globals, macros, and library names. The line under the prompt shows the signature of the call the cursor sits inside, or the first diagnostic in what you have typed so far; it is produced by parsing the input without executing it. History defaults to `~/.cterpreter_history`. Editing assumes single-column characters and works best with lines narrower than the terminal.

| Command | Effect |
| --- | --- |
| `.help`, `.version`, `.quit` | Help, version, exit |
| `.clear` | Reset variables, functions, types, macros, and allocations |
| `.vars` or `.dump` | Inspect globals, functions, memory, and limits |
| `.depth` | Show how many `interpret()` calls deep this instance is |
| `.type EXPR` | Inspect a type without executing the expression |
| `.ast SOURCE` | Display the syntax tree, with declared types, without executing it |
| `.source` | Display accepted session source |
| `.load FILE` | Execute a file in the current session |
| `.save FILE` | Save accepted session source |
| `.restore FILE` | Replay source into a fresh session |

Save/restore is source replay, not a memory snapshot. Replay repeats I/O and other side effects, and input-dependent programs can produce different state. A failed restore retains the previous interpreter. Saved submissions receive statement separators so bare REPL expressions can be replayed.

## Files and command-line options

```sh
./build/Cterpreter examples/basics/hello.c Ada
./build/Cterpreter -e 'printf("%d\n", 6 * 7);'
printf 'int x = 6; x * 7\n' | ./build/Cterpreter
./build/Cterpreter --no-prompt
./build/Cterpreter --prompt 'C → ' --color always --no-history
```

Files and piped source run `main` automatically when it exists. Both `int main(void)` and `int main(int argc, char **argv)` are supported; the return value becomes the process exit status. Top-level Cterpreter statements are also allowed. Source supplied with `-e` evaluates directly, without automatically calling `main`.

`--no-prompt` selects a line-oriented REPL even with redirected input. Other options include `--history FILE`, `--color auto|always|never`, `--max-steps N`, `--max-depth N`, `--max-nesting N`, `--strict`, and `--verbose`. `NO_COLOR` disables automatic colors. `--help` lists all options.

`--strict` turns an invalid memory access into the `SIGSEGV` the program would have earned natively, after printing the diagnostic, instead of reporting it and stopping cleanly.

## Nested interpreters

`interpret("path.c")` runs a program inside a child interpreter with its own symbols, memory, and macros. The child shares the caller's streams and interrupt flag, inherits the caller's remaining step budget, and returns the nested program's exit status. `interpret_depth()` reports how deep the current instance is, and `--max-nesting` bounds the chain.

```sh
./build/Cterpreter -e 'interpret("examples/basics/hello.c");'
```

This is nesting, not self-interpretation: Cterpreter cannot yet run its own source.

## Implemented language and runtime

- `_Bool`, plain/signed/unsigned `char`, `short`, `int`, `long`, `long long` with their unsigned counterparts, `float`, `double`, and `void`.
- Integer promotions and the usual arithmetic conversions. Unsigned arithmetic wraps; signed overflow is refused rather than wrapped. Literals take their type from their value and their `u`/`l` suffixes.
- Pointers, multidimensional arrays, structures, unions, enumerations, function types, and function pointers, with member access through `.` and `->`, indirect calls, and C's layout and alignment rules.
- Full declarator syntax, including arrays of pointers, pointers to arrays, and pointers to functions, plus `typedef`, `const`, `volatile`, `restrict`, `extern`, `register`, `inline`, and `auto`. Tags and typedefs may be declared at block scope.
- Initializer lists with brace elision, designated initializers for members and array elements, string initialization, and compound literals.
- Aggregate assignment, aggregate arguments, and aggregate return values, all copied by value.
- Casts, `sizeof` (yielding `size_t`), `_Alignof`, `_Generic`, and `offsetof`.
- Arithmetic, comparison, logical, bitwise, shift, assignment, increment/decrement, and conditional operators.
- Lexical scopes, globals, static locals, `const` objects, and multiple declarators per declaration.
- Function definitions, prototypes, recursion, and void functions.
- `if`/`else`, `while`, `do-while`, `for`, `switch` with direct case labels, `break`, `continue`, `return`, and jumps to labels in the same or an enclosing block.
- Address-of, dereferencing, subscripting, pointer arithmetic, and a managed address space with object lifetimes.
- `malloc`, `calloc`, `realloc`, `free`, and diagnostics for null pointers, bounds violations, uninitialized reads, invalid frees, expired objects, and writes to string literals.
- `#include`, `#define`, `#undef`, conditional directives, function-like/variadic macros, stringification, token concatenation, `#pragma once`, `#line`, and `#error`.
- The predefined macros `__FILE__`, `__LINE__`, `__DATE__`, `__TIME__`, `__COUNTER__`, `__STDC__`, `__STDC_VERSION__`, `__STDC_HOSTED__`, the `__STDC_NO_*` feature macros, `__CTERPRETER__`, and `__CTERPRETER_VERSION__`.

Quoted headers resolve relative to the source file. Supported standard includes are `stdio.h`, `stdlib.h`, `string.h`, `math.h`, `ctype.h`, `stddef.h`, `stdint.h`, `stdbool.h`, `limits.h`, `float.h`, `time.h`, `errno.h`, `assert.h`, and `iso646.h`; they expose the implemented runtime subset with the host's real limits, not the host system headers. Include guards and `#pragma once` both work. Macro expansion remains a subset of the C17 preprocessing rules.

The runtime includes:

- `printf`, `fprintf`, `puts`, `putchar`, `getchar`, `sprintf`, and `snprintf`.
- `scanf`, `fscanf`, and `sscanf` with `h`, `hh`, `l`, `ll`, and `z` destinations, and destination bounds checks.
- File opening/closing, line and block I/O, seeking, flushing, EOF/error queries, `ungetc`, `remove`, and `rename`. Open files are closed when the interpreter is cleared or destroyed.
- `stdin`, `stdout`, `stderr`, an assignable `errno`, `perror`, `strerror`, and `getenv`. Standard streams remain owned by the interpreter.
- `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn`, `strpbrk`, `strtok`, `memcpy`, `memmove`, `memset`, `memcmp`, and `memchr`.
- Allocation functions, `atoi`, `atol`, `atof`, `strtol`, `strtod`, `abs`, `labs`, `rand`, `srand`, `exit`, `abort`, and `assert`.
- `qsort` and `bsearch`, which call interpreted comparison functions through function pointers.
- Math functions, including roots, powers, trigonometric and hyperbolic functions, logs, rounding, `fmod`, `fmin`/`fmax`, and `copysign`.
- Character classification/conversion functions, `time`, `difftime`, and `clock`.

`printf` supports integer, floating-point, character, string, and pointer conversions, flags, field widths, precision, `*` arguments, and the `h`, `hh`, `l`, `ll`, `z`, `j`, and `t` length modifiers, which select the type the argument is read as. Arguments are checked before being passed to host formatting functions. Wide characters and `long double` formats are not supported.

## Current limits

This is not a conforming C17 implementation. Remaining work includes bit-fields, `long double`, complex numbers, atomics, variable-length arrays, variadic interpreted functions, `va_list`, arbitrary jumps into nested blocks, and self-interpretation. `volatile` and `restrict` are accepted and ignored; `extern` declares rather than references. Array bounds must be integer constant expressions the parser can fold. Pointer values use a virtual 64-bit address representation, not host addresses, so casting a pointer to an integer yields that virtual address.

Semantic checking largely happens during execution. Operand/argument evaluation proceeds left to right, and unsequenced side effects are not diagnosed. Some invalid constructs in unexecuted branches can therefore escape checking. Switch labels belong directly to the switch block. Diagnostics after expanded includes can refer to the expanded source line rather than the original header line.

There is no dynamic-library interface, clangd/LSP connection, native assembly backend, or debugger. Completions, signatures, and live diagnostics come from Cterpreter's own symbol table and parser, not from clangd. The standard library is a subset; scanf scansets and wide-character formats are not implemented. `.type` and `.ast` use the current macros and type aliases without executing the inspected source.

Defaults are 1 MiB of source per submission, 64 MiB of live managed object data, one million execution steps, 256 evaluation frames, and 8 levels of `interpret()` nesting. There are separate syntax/macro/include nesting limits and a 4096-entry limit on distinct types. These are resource bounds, not a security sandbox. Interpreter instances have separate symbols, macros, memory, file handles, and random-number state; the type descriptions themselves are shared and released when the last instance is destroyed.

## Development

`include/cterpreter.h` exposes the embedding API. The lexer, parser, type registry, preprocessor, evaluator, managed memory, library adapters, boot ceremony, and terminal editor live in `src/`. Host streams, execution limits, nesting, and strict mode are configurable per interpreter.

Strict Clang/GCC warnings are enabled. The test suite covers the lexer and parser directly, pins integer boundaries and conversions, fuzzes malformed input, and compares every example against the host compiler's own output. CI is configured for GCC and Clang on Linux and Apple Clang on macOS; these hosted runs have not been executed in this workspace.

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build/sanitize -DCT_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sanitize
ctest --test-dir build/sanitize --output-on-failure
```

`-DCT_REPRODUCIBLE_TEST=ON` adds a check that builds the core library twice from different paths and compares the archives. `-DCT_FUZZER=ON` builds the fuzz target with libFuzzer instead of its generated corpus:

```sh
cmake -S . -B build/fuzz -DCT_FUZZER=ON -DCMAKE_C_COMPILER=clang
cmake --build build/fuzz
./build/fuzz/fuzz_tests -max_total_time=60
```
