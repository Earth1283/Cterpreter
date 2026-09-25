# Examples

37 programs, grouped by their main feature. Many deliberately combine several features; the table below lists those connections. Run commands from the repository root after building Cterpreter.

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
./build/Cterpreter examples/types/variadic.c
./build/Cterpreter --max-steps 900000000 examples/playground/donut.c
```

For the more involved programs, start with merge sort, N-Queens, the growing vector, shortest paths, or the stack machine. For the type system, start with the aggregate, function-pointer, and numeric-type programs. For something to play with rather than read, go to [the playground](#playground). For something that should not exist, go to [unhinged](#unhinged).

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
| [types/variadic.c](types/variadic.c) | Variadic function pointers, integer/float promotions, `va_list`, `va_copy`, formatting in two passes, `vsscanf`, aggregate arguments and returns | `sum = 42`; `promoted float: 1.25 / 42`; `scan = 3: 42 2.5 arguments`; `points = (7, 11)` |
| [io/csv_report.c](io/csv_report.c) | `fopen`, `fgets`, `sscanf`, account aggregation, floating-point formatting | 6 transactions, 3 accounts, total 125.00 |
| [io/file_roundtrip.c](io/file_roundtrip.c) | Exclusive file creation, block I/O, flushing/seeking, `memcmp`, cleanup via `goto` | Restores five integers with checksum 131; removes its file |
| [io/calculator.c](io/calculator.c) | Formatted stdin, `switch`, EOF, recoverable division-by-zero handling | Prints results for supplied calculations |
| [playground/donut.c](playground/donut.c) | Rotating-torus raymarching, `math.h`, z-buffering, ANSI cursor control | A spinning ASCII donut, 48 frames by default |
| [playground/life.c](playground/life.c) | Toroidal grids, double buffering, neighbour counting, pattern seeding | A Gosper glider gun firing gliders that wrap and collide |
| [playground/brainfuck.c](playground/brainfuck.c) | An interpreter inside an interpreter: bracket matching, a tape, a dispatch loop | `Hello World!`, the first eleven squares, A–Z, or a Sierpinski triangle |
| [playground/maze.c](playground/maze.c) | Recursive backtracking, an explicit direction shuffle, recursive DFS solving | A carved maze, then the same maze with its route marked in dots |
| [playground/adventure.c](playground/adventure.c) | Structures, function-pointer verb dispatch, `strtok`, `fgets`, game state | A small text adventure set inside a C interpreter |
| [playground/toolkit.c](playground/toolkit.c) | A dozen callable curiosities meant for `.load` rather than for running | Prints a menu of things to call from the REPL |
| [unhinged/quine.c](unhinged/quine.c) | Escaping, string tables, self-reference | Its own source, byte for byte |
| [unhinged/inception.c](unhinged/inception.c) | `interpret()`, `interpret_depth()`, exit statuses, `__CTERPRETER__` | `5! = 120`, computed six interpreters deep |
| [unhinged/forth.c](unhinged/forth.c) | A Forth compiler and threaded-code VM: a dictionary, control-flow patching, return stacks | Factorials, primes, FizzBuzz, and a triangle of stars, all in Forth |
| [unhinged/lisp.c](unhinged/lisp.c) | A Lisp with closures: a reader, a cell pool, interned symbols, `eval`/`apply` | Maps, a prime sieve, and `10!` through the Y combinator |
| [unhinged/ski.c](unhinged/ski.c) | Graph reduction of S, K, and I combinators, Church numerals, in-place redex updates | `(3 * 3) ^ 2 = 81` with no numbers until the end |
| [unhinged/pointer_chase.c](unhinged/pointer_chase.c) | A shuffled chain of `void *` rooms, pointer subtraction, a 46-level pointer type | The route through 45 rooms, then `46 stars later: 42` |

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

## Playground

These are here to be played with. Several want more than the default execution budget, so start the interpreter with a larger one:

```sh
./build/Cterpreter --max-steps 900000000 examples/playground/donut.c 48
./build/Cterpreter --max-steps 900000000 examples/playground/life.c gun 200
./build/Cterpreter --max-steps 900000000 examples/playground/life.c soup 150 42
./build/Cterpreter --max-steps 900000000 examples/playground/brainfuck.c triangle
./build/Cterpreter --max-steps 900000000 examples/playground/brainfuck.c '++++++++++[>++++++<-]>++++.---.+++++++..'
./build/Cterpreter examples/playground/maze.c 7
./build/Cterpreter --max-depth 8192 --max-steps 900000000 examples/playground/maze.c 7 79 39
./build/Cterpreter examples/playground/adventure.c
```

The donut animates in place, so give the terminal at least 24 rows. Life takes a pattern name — `gun`, `acorn`, `r`, `diehard`, or `soup` with a seed. Brainfuck takes a catalogue name — `hello`, `squares`, `alphabet`, `triangle` — or any Brainfuck program as a single argument. The maze takes a seed and optional odd dimensions; the big one recurses deeper than the default frame budget allows.

The adventure is played at a prompt. You wake up inside a C interpreter and have to get out through `main()`; `help` lists the verbs. Entering Undefined Behaviour without a cast ends about as well as it does in real C.

The toolkit is different: it is meant to be loaded into a live session rather than run.

```sh
./build/Cterpreter --max-steps 900000000
c> .load examples/playground/toolkit.c
c> mandel(-0.75, 0.0, 3.0)
c> mandel(-0.745, 0.113, 0.02)
c> bifurcation()
c> collatz(27)
c> roman(1987)
c> hanoi(4, 'A', 'C', 'B')
c> bogosort(6, 1)
c> ackermann(3, 6)
```

Loading it prints the menu and leaves every function defined in the session, so you can call them with your own arguments, redefine them, or build on them. `ackermann(3, 6)` is included because it is the fastest way to meet the step limit on purpose.

## Unhinged

Programs that are here because they could be, not because they should be. Each runs with the default limits.

```sh
./build/Cterpreter examples/unhinged/quine.c | diff - examples/unhinged/quine.c
./build/Cterpreter examples/unhinged/inception.c
./build/Cterpreter examples/unhinged/forth.c
./build/Cterpreter examples/unhinged/lisp.c
./build/Cterpreter examples/unhinged/ski.c
./build/Cterpreter examples/unhinged/pointer_chase.c
```

The quine prints its own source; the `diff` above prints nothing. Inception computes a factorial by running itself: each level starts the same file in a nested interpreter and gets the smaller factorial back as that program's exit status. Compiled natively, it has nowhere to go and says so. The Forth and the Lisp are complete little languages, each running a program of its own, and all of it happens inside Cterpreter. The SKI program does arithmetic with three combinators and no numbers, and only turns its Church numerals into digits to print them. The pointer chase threads 45 shuffled rooms into one chain, walks it with a loop, and then reaches the treasure the other way: with `**********************************************px`.

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

31 of the successful programs were compiled as C17 with GCC on Linux and their stdout, stderr, and exit statuses compared against Cterpreter using the documented sample inputs. All matched byte for byte, the donut included, which pins several thousand floating-point results at once. The three diagnostic examples produced their expected errors. The comparison runs as the `reference_programs` CTest check, so it is repeated on every test run.

The maze and Life's random soup are excluded from that comparison, and so is inception, which only runs its computation under Cterpreter. `rand()` is Cterpreter's own generator, kept per interpreter instance so that nested instances stay independent, so a seeded run is reproducible in Cterpreter but does not match the host's sequence.

These examples demonstrate supported behavior; they do not establish full C17 conformance.
