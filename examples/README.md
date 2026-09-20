# Examples

24 programs, grouped by their main feature. Many deliberately combine several features; the table below lists those connections. Run commands from the repository root after building Cterpreter.

```sh
./build/Cterpreter examples/basics/hello.c Ada
./build/Cterpreter examples/recursion/merge_sort.c
./build/Cterpreter examples/recursion/n_queens.c
./build/Cterpreter examples/memory/dynamic_vector.c
./build/Cterpreter examples/memory/shortest_path.c
./build/Cterpreter examples/control_flow/stack_machine.c
./build/Cterpreter examples/types/aggregates.c
./build/Cterpreter examples/types/function_pointers.c
./build/Cterpreter examples/types/numeric_types.c
```

For the more involved programs, start with merge sort, N-Queens, the growing vector, shortest paths, or the stack machine. For the type system, start with the aggregate, function-pointer, and numeric-type programs.

## Program index

| Example | Features exercised | Expected result with default input |
| --- | --- | --- |
| [basics/hello.c](basics/hello.c) | `main`, arguments, strings, conditional expressions, `printf` | `Hello, world!`, or the supplied name |
| [recursion/factorial.c](recursion/factorial.c) | Recursive calls, integer arithmetic, return values | `10! = 3628800` |
| [recursion/merge_sort.c](recursion/merge_sort.c) | Divide-and-conquer recursion, scratch allocation, pointer parameters, merging | `-19 -4 0 3 5 9 10 27 27 38 43 82` |
| [recursion/mutual_recursion.c](recursion/mutual_recursion.c) | Prototypes, mutually recursive functions, loops | Even/odd classifications for 0–9 |
| [recursion/n_queens.c](recursion/n_queens.c) | Backtracking, recursion, global arrays, pruning | First placement `1 3 5 0 2 4`; `6 queens: 4 solutions` |
| [control_flow/state_machine.c](control_flow/state_machine.c) | Enums, `switch`, transitions, `continue`, strings | A coin-operated machine ends with state 1, credit 1, and one drink |
| [control_flow/stack_machine.c](control_flow/stack_machine.c) | Instruction decoding, a stack, fallthrough cases, loops, error jumps | Evaluates `((7 + 5) * 3 - 4)^2`; prints `stack[0] = 1024` |
| [memory/squares.c](memory/squares.c) | `calloc`, subscripting, loops, `free` | `0, 1, 4, 9, 16` |
| [memory/dynamic_vector.c](memory/dynamic_vector.c) | Pointer-to-pointer parameters, `realloc`, capacity growth, overlapping `memmove` | Length 8, capacity 16; `1 4 16 25 36 49 64 81` |
| [memory/shortest_path.c](memory/shortest_path.c) | Dijkstra's algorithm, a flat adjacency matrix, parallel arrays, path reconstruction | Cost 20 to node 4; route `0 2 5 4` |
| [memory/sieve.c](memory/sieve.c) | Heap-backed flags, nested loops, argument parsing | 25 primes through 100 |
| [strings/word_frequency.c](strings/word_frequency.c) | In-place tokenization, character classification, arrays of pointers, sorting | Alphabetical counts: `c: 3`, `debugging: 1`, `interesting: 2`, `make: 1`, `makes: 2`, `pointers: 2` |
| [strings/run_length.c](strings/run_length.c) | Encoding/decoding, decimal parsing, bounded formatting, buffer checks | Encodes as `a4b3c2d1e5`; round trip `OK` |
| [preprocessor/macros.c](preprocessor/macros.c) | Local includes, include guards, conditional directives, variadic/nested macros, stringification, token pasting | `answer = 42`; nested expansion 49 |
| [types/typedefs_generics.c](types/typedefs_generics.c) | Typedefs, enums, `_Generic`, static locals, `sizeof`, `_Alignof` | Numeric/text type selection; IDs 101 and 102 |
| [types/aggregates.c](types/aggregates.c) | Nested structures, unions, arrays of structures, struct copies and returns, designated and elided initializers, multidimensional arrays, `->` | Length squared 25; grid total 45 in 48 bytes; `sparse first (0, 0) last (7, 8)` |
| [types/function_pointers.c](types/function_pointers.c) | Function pointers in structures, indirect calls, pointer-to-function parameters, `qsort` and `bsearch` with interpreted comparators | `mul -> 24`; descending `88 42 23 19 7 3`; `found 19 at index 3` |
| [types/numeric_types.c](types/numeric_types.c) | The whole numeric tower, integer promotions, usual arithmetic conversions, unsigned wraparound, signed/unsigned comparison, `float` versus `double`, shifts and masks, `limits.h`, `stdint.h`, `stdbool.h` | `sizes 1 1 2 4 8 8 4 8`; `unsigned wraps to 4294967295 and back to 0`; `mask deadbeef rotated beefdead` |
| [io/csv_report.c](io/csv_report.c) | `fopen`, `fgets`, `sscanf`, account aggregation, floating-point formatting | 6 transactions, 3 accounts, total 125.00 |
| [io/file_roundtrip.c](io/file_roundtrip.c) | Exclusive file creation, block I/O, flushing/seeking, `memcmp`, cleanup via `goto` | Restores five integers with checksum 131; removes its file |
| [io/calculator.c](io/calculator.c) | Formatted stdin, `switch`, EOF, recoverable division-by-zero handling | Prints results for supplied calculations |

The graph is stored in one flat array using `row * NODES + column`, and the vector uses separate pointer, length, and capacity variables; both predate aggregate support and still run unchanged. The `types` programs use structures and multidimensional arrays directly.

## Inputs and files

N-Queens accepts a board size from 1 to 7. Six queens is the default. Larger searches can use an increased execution budget:

```sh
./build/Cterpreter --max-steps 10000000 examples/recursion/n_queens.c 7
./build/Cterpreter examples/memory/sieve.c 1000
```

The sieve accepts limits from 2 to 5000. The string programs use built-in ASCII samples; the run-length format is a letter followed by a positive decimal count.

The CSV program defaults to the bundled numeric fixture. It also accepts a path to a file with the same `account,amount` header and numeric row format:

```sh
./build/Cterpreter examples/io/csv_report.c examples/io/fixtures/transactions.csv
```

The file round trip requires a new path. It refuses to overwrite an existing file and removes the file it created during cleanup:

```sh
./build/Cterpreter examples/io/file_roundtrip.c /tmp/cterpreter-example.bin
```

The calculator reads stdin until EOF:

```sh
printf '12 + 7\n9 / 2\n5 * 6\n8 / 0\n' | ./build/Cterpreter examples/io/calculator.c
```

It prints 19, 4, and 30 for the first three calculations, then reports the division by zero and keeps reading. When entering input interactively, Ctrl+D finishes.

## Intentional diagnostics

These three programs are expected to fail with exit status 1. They demonstrate Cterpreter diagnostics, so they are excluded from comparisons with native execution.

| Example | Expected diagnostic |
| --- | --- |
| [diagnostics/out_of_bounds.c](diagnostics/out_of_bounds.c) | `access outside object bounds` |
| [diagnostics/use_after_free.c](diagnostics/use_after_free.c) | `access to an object whose lifetime has ended` |
| [diagnostics/integer_overflow.c](diagnostics/integer_overflow.c) | `signed integer overflow` |

```sh
./build/Cterpreter examples/diagnostics/use_after_free.c
```

## Verification

The 21 successful programs were compiled as C17 with GCC on Linux and their stdout, stderr, and exit statuses compared against Cterpreter using the documented sample inputs. All matched byte for byte. The three diagnostic examples produced their expected errors. The comparison runs as the `reference_programs` CTest check, so it is repeated on every test run. These examples demonstrate supported behavior; they do not establish full C17 conformance.
