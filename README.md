# Cterpreter

C, but interpreted. A C17 implementation of a C interpreter, with a persistent REPL and an increasingly unreasonable [roadmap](Cterpreter_Roadmap.md).

Cterpreter evaluates its own AST; it does not invoke a compiler to execute your program. It now runs small C programs with functions, pointers, arrays, macros, and standard I/O. Full C17 and self-interpretation remain unfinished.

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
c> int values[] = {10, 20, 30};
c> int *p = values;
c> p[1] += 2;
c> values[1]
22
c> int factorial(int n) {
...     if (n < 2) return 1;
...     return n * factorial(n - 1);
... }
c> factorial(6)
720
c> .type p
int*
```

An expression without a final semicolon prints its value. A semicolon suppresses that result, so `printf("hello\n");` prints just `hello`. Variables, functions, macros, and allocated objects persist between submissions.

Incomplete syntax continues at `...`. Ctrl+C cancels input or execution, and Ctrl+D exits. Syntax errors leave the existing session intact. Runtime errors preserve effects that already completed.

The terminal supports arrows, Home/End, insertion, deletion, Ctrl+A/E, Ctrl+K/U/W, history recall, and Ctrl+R to search earlier history for the current text. Tab accepts a built-in keyword/function suggestion; colored terminals show it inline. History defaults to `~/.cterpreter_history`. Editing currently assumes single-column characters and works best with lines narrower than the terminal.

| Command | Effect |
| --- | --- |
| `.help`, `.version`, `.quit` | Help, version, exit |
| `.clear` | Reset variables, functions, macros, and allocations |
| `.vars` or `.dump` | Inspect globals, functions, memory, and limits |
| `.type EXPR` | Inspect a type without executing the expression |
| `.ast SOURCE` | Display the syntax tree without executing it |
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

`--no-prompt` selects a line-oriented REPL even with redirected input. Other options include `--history FILE`, `--color auto|always|never`, `--max-steps N`, `--max-depth N`, and `--verbose`. `NO_COLOR` disables automatic colors. `--help` lists all options.

## Implemented language and runtime

- `int`, `double`, `char`, `void`, pointer types, numeric conversions, casts, `sizeof`, basic `_Alignof`, and `_Generic`.
- Numeric, character, and string literals, escape sequences, and adjacent string concatenation.
- Arithmetic, comparison, logical, bitwise, shift, assignment, increment/decrement, and conditional operators.
- Lexical scopes, globals, static locals, scalar `const` objects, multiple declarators, one-dimensional arrays, brace initializers, and string initialization.
- Global scalar/pointer typedefs and enums with integer constant expressions.
- Function definitions, prototypes, recursion, numeric/pointer parameters and returns, and void functions.
- `if`/`else`, `while`, `do-while`, `for`, `switch` with direct case labels, `break`, `continue`, `return`, and jumps to labels in the same or an enclosing block.
- Address-of, dereferencing, subscripting, pointer arithmetic, and a managed address space with object lifetimes.
- `malloc`, `calloc`, `realloc`, `free`, and diagnostics for null pointers, bounds violations, uninitialized reads, invalid frees, expired objects, and writes to string literals.
- `#include`, `#define`, `#undef`, conditional directives, function-like/variadic macros, stringification, token concatenation, `__FILE__`, and `__LINE__`.

Quoted headers resolve relative to the source file. Supported standard includes are `stdio.h`, `stdlib.h`, `string.h`, `math.h`, `ctype.h`, `stddef.h`, `limits.h`, `time.h`, and `errno.h`; these expose the implemented runtime subset, not the host system headers. Include guards work. Macro expansion remains a subset of the C17 preprocessing rules.

The runtime includes:

- `printf`, `fprintf`, `puts`, `putchar`, `getchar`, `sprintf`, and `snprintf`.
- `scanf`, `fscanf`, and `sscanf` for integers, characters, strings, and `%lf` doubles, with destination bounds checks.
- File opening/closing, line and block I/O, seeking, flushing, EOF/error queries, `remove`, and `rename`. Open files are closed when the interpreter is cleared or destroyed.
- `stdin`, `stdout`, `stderr`, file-error `errno`, `strerror`, and `getenv`. Standard streams remain owned by the interpreter.
- `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`, `strcat`, `strchr`, `strstr`, `memcpy`, `memmove`, `memset`, and `memcmp`.
- Allocation functions, `atoi`, `atof`, `abs`, `rand`, `srand`, and `exit`.
- Common math functions, including `sqrt`, `pow`, trigonometric functions, logs, rounding, and `fmod`.
- Common character classification/conversion functions and `clock`.

`printf` supports integer, floating-point, character, string, and pointer conversions, flags, field widths, precision, and `*` arguments. Arguments are checked before being passed to host formatting functions. Wide characters and `long double` formats are not supported.

## Current limits

This is not a conforming C17 implementation. Remaining work includes structures/unions, complex declarators, local typedef/enum definitions, multidimensional arrays, designated initializers, compound literals, function pointers, arbitrary jumps into nested blocks, atomics, full qualifiers, and the wider numeric type system. `size_t` currently maps to the supported `int` type. Pointer values use a virtual 64-bit address representation, not host addresses.

Semantic checking largely happens during execution. Operand/argument evaluation proceeds left to right, and unsequenced side effects are not diagnosed. Some invalid constructs in unexecuted branches can therefore escape checking. Switch labels currently belong directly to the switch block. `_Alignof` uses the supported scalar layout. Diagnostics after expanded includes can refer to the expanded source line rather than the original header line.

There is no dynamic-library interface, clangd/LSP connection, debugger, or self-interpretation yet. The standard library is a subset; for example, scanf scansets and wide-character formats are not implemented. `errno` is currently readable rather than an assignable C lvalue. Tab suggestions come from a built-in vocabulary. `.type` and `.ast` use the current macros and global type aliases without executing the inspected source.

Defaults are 1 MiB of source per submission, 64 MiB of live managed object data, one million execution steps, and 256 evaluation frames. There are separate syntax/macro/include nesting limits. These are resource bounds, not a security sandbox. Interpreter instances have separate symbols, macros, memory, file handles, and random-number state.

## Development

`include/cterpreter.h` exposes the embedding API. The lexer, parser, preprocessor, evaluator, managed memory, library adapters, and terminal editor live in `src/`. Host streams and execution limits are configurable per interpreter.

Strict Clang/GCC warnings are enabled. Focused smoke checks and optional sanitizers keep development centered on working language features. CI is configured for GCC and Clang on Linux and Apple Clang on macOS; these hosted runs have not been executed in this workspace.

```sh
ctest --test-dir build --output-on-failure
cmake -S . -B build/sanitize -DCT_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/sanitize
ctest --test-dir build/sanitize --output-on-failure
```
