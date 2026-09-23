# Misendeavor log

## 2026-09-23 — 0.3.1: interpreter performance

Profiling with callgrind showed the time going to bookkeeping rather than to the programs: every local variable cost two host allocations, every name use rehashed its text and walked the scope chain, builtins were dispatched by comparing name strings, and `qsort` was a shell sort that called the interpreted comparator O(n^1.5) times. Measured as best of five runs against the previous commit, the benchmark set (ten micro-benchmarks plus the playground) went from 10.6 s to 3.9 s, **2.7× faster** overall: the donut 6.6 s → 2.4 s, Life 1.9 s → 0.76 s, recursive `fib(25)` 2.5×, and a 20,000-element `qsort` 4.6×. All diagnostics, strict-mode behaviour, and example outputs are unchanged.

- Scalars and small aggregates keep their bytes inside the allocation record, and records are recycled once forgotten, so declaring a local no longer touches the host allocator. Lookups check the most recently used record first and binary-search a contiguous address array.
- A name's resolution is cached on its node and revalidated by scope-instance serial and symbol count. Each scope carries a 128-bit filter of its declared names, and each local declaration knows its innermost live symbol, so a cached resolution survives a block being re-entered on every loop iteration without rescanning the chain.
- Scalar reads and writes through a variable decode its record directly; `int` and `double` arithmetic take a fast path; `switch` labels are validated and evaluated once instead of pairwise on every execution; the type of a `?:` expression and a `goto`'s label are cached.
- Builtins dispatch on an integer id. `qsort` is a stable bottom-up merge sort, which matches glibc's order for equal keys; the shell sort remains as a fallback when the scratch buffer would exceed the memory limit. `printf` copies literal runs in one step and formats each conversion once.
- Function pointers resolve by binary search, the lexer matches operators with a switch instead of 22 string comparisons, and the preprocessor emits punctuation runs whole.

### Parsing its own source

Cterpreter could not get past the first lines of its own files. It now parses and loads all seven files of its interpreter core, plus `boot.c`. That needed `signal.h`, `inttypes.h`, and the rest of `stdint.h`; `_Static_assert`; the comma operator; repeated file-scope declarations joined by `extern`; and `sizeof` of earlier-declared objects folded in constant expressions. Including a header that carries declarations no longer shifts diagnostic line numbers. Running itself still needs the POSIX headers the command-line front end uses, and a way to link several source files into one program.

## 2026-09-21 — CLI preferences and presentation

The REPL now has a `.config` command with independent controls for plain-language syntax tips, automatic highlighting, suggestions/Tab completion, function signatures, live diagnostics, and the color mode. Changes apply immediately. `.config reset` restores defaults in memory; `.config save` writes `~/.cterpreterrc` through a temporary file and rename. Startup reads that file, `--config FILE` selects another path, `--no-config` skips loading, and `--color` overrides a saved color mode. Invalid files are rejected without partially applying their settings.

The CLI has a cleaner banner and settings display, a consistent cyan prompt, and colors for types, keywords, function calls, numeric literals, strings, and comments. Highlighting follows multiline comment/string context and remains visible after submitting a line. Long input scrolls horizontally; hints and suggestions are clipped to the terminal width. Automatic colors respect `NO_COLOR` and `TERM=dumb`.

The status line labels function signatures, syntax diagnostics, and readable tips. Tips include control-flow templates and explanations for common parser errors. Checks use pending multiline source, and signature lookup ignores parentheses inside strings and comments. `.config` names and values support Tab completion. These preferences survive `.clear` and source replay without affecting program execution or its resource limits.

The new optional Python 3 CTest check exercises saved preferences, invalid configurations, command-line overrides, tips, and the actual terminal editor through a pseudo-terminal. It verifies live feature toggles, multiline comment highlighting, completion, signatures, `NO_COLOR`, and editing a 121-byte expression in a 32-column terminal. All **9 CTest checks pass** in the normal and AddressSanitizer/UndefinedBehaviorSanitizer builds; the final config-completion addition also passes the targeted CLI check in both builds.

## 2026-09-21 — Work since 0.3.0: variadic functions

Interpreted functions can now accept `...`, including calls through function pointers. `stdarg.h` supplies `va_list`, `va_start`, `va_arg`, `va_copy`, and `va_end`. Unnamed arguments receive the default integer and floating-point promotions; aggregate arguments are copied by value. Recursive calls have separate argument frames, and lists can be forwarded to helpers or copied for independent traversal.

The seven formatted-I/O adapters `vprintf`, `vfprintf`, `vsprintf`, `vsnprintf`, `vscanf`, `vfscanf`, and `vsscanf` reuse the existing checked format engines with interpreted arguments. A list consumed by these adapters must be ended; a copy made beforehand supports a second pass. No host `va_list` or host address is exposed to the interpreted program.

### Diagnostics and supporting fixes

- `va_start` checks that it runs in a variadic function and names its actual last parameter, including shadowing checks.
- `va_arg` diagnoses exhausted lists and mismatched promoted types, while accepting the standard's representable signed/unsigned and character-pointer/void-pointer exceptions.
- Ended, uninitialized, expired, and formatted-I/O-consumed lists are diagnosed. Argument frames unwind on runtime errors and evaluation-limit failures, leaving the REPL usable.
- Built-in headers now preserve ordinary declarations as well as macro definitions; previously the header path discarded declarations, including the new `va_list` typedef.
- `sizeof`, `_Generic`, type inspection, and AST dumps recognize the stdarg intrinsics without consuming arguments during inspection.

### Verification

- All **8 CTest checks pass** in both the normal GCC build and the AddressSanitizer/UndefinedBehaviorSanitizer build.
- The new varargs suite covers promotions, indirect and recursive calls, aggregate copies, helper forwarding, independent list cursors, single evaluation of list operands, diagnostics, error recovery, all seven formatted-I/O adapters, and clearing/reusing a session.
- Parser tests cover malformed intrinsics and typed AST output; the fuzz corpus now includes stdarg intrinsics.
- [examples/types/variadic.c](examples/types/variadic.c) matches the host compiler's output and exit status in the reference comparison. The example collection now contains **31 programs**.

This closes the variadic-function and `va_list` gaps listed in the 0.3.0 checkpoint below. The other unfinished language features and self-interpretation remain open; this is not a new release or a claim of complete C17 conformance.

## 2026-09-21 — Current state: 0.3.0

The interpreter gained a real type system. Types are now interned descriptions in a shared registry rather than an arithmetic encoding of a scalar kind, which made structures, unions, multidimensional arrays, function types, and complex declarators expressible for the first time. On top of that, the full C numeric tower and its conversion rules landed, and the REPL's ghost text, signature hints, and diagnostics now come from the live session instead of a fixed word list.

### New since 0.2.0

- A refcounted global type registry describing every object and function type, with C's layout, alignment, and padding rules, and declarator-syntax type names such as `int (*)(int, char **)`.
- Structures, unions, nested aggregates, arrays of aggregates, member access through `.` and `->`, and aggregate assignment, arguments, and return values, all copied by value.
- Multidimensional arrays, arrays of pointers, pointers to arrays, pointers to functions, and the rest of C's declarator grammar, including `typedef` and tag declarations at block scope.
- Initializer lists with brace elision, designated member and array initializers, string initialization, and compound literals.
- Function pointers and indirect calls, including `qsort` and `bsearch` calling interpreted comparison functions.
- `_Bool`, the signed and unsigned char/short/int/long/long long family, and `float`, with the integer promotions and usual arithmetic conversions of C17 6.3.1.1 and 6.3.1.8. Literals take their type from their value and suffix. Unsigned arithmetic wraps; signed overflow is still refused rather than wrapped.
- `stdbool.h`, `stdint.h`, `float.h`, `assert.h`, and `iso646.h`; the predefined macros, `#pragma once`, `#line`, and `__COUNTER__`; `errno` as an assignable object that library functions actually set; real prototypes for every library function.
- Nested interpreter instances through `interpret()`, sharing a step budget, with `interpret_depth()`, `.depth`, and `--max-nesting`.
- `--strict`, which raises the `SIGSEGV` an invalid access would have earned natively after printing the diagnostic.
- Completions drawn from the session's own globals, macros, and library names; a signature hint for the call the cursor sits inside; and the first diagnostic in the current line, all produced by parsing the input without executing it.
- The ceremonial `volatile` inline-assembly `nop` and 2 + 2 verification through AVX-512 ZMM lanes or ARM NEON lanes where the host provides them.
- A `playground` folder of six programs written to be played with rather than read: a rotating ASCII donut, Conway's Game of Life with a Gosper glider gun, a Brainfuck interpreter running inside this one, a recursive maze generator and solver, a text adventure set inside a C interpreter, and a toolkit of curiosities meant to be `.load`ed into a live session.

### Fixed while building the playground

- Object lookup was a linear walk of every allocation ever made, and freed objects were never reclaimed, so any program that declared a variable inside a loop was quadratic in the number of iterations. Objects now live in a table kept sorted by address and found by binary search, and all but the most recent few thousand dead entries are dropped. A loop of 16,000 iterations declaring two locals went from 2.7 seconds to 0.02; one frame of the donut went from 26 seconds to 0.16.
- `--max-depth` counts nested AST evaluations rather than function calls, so one level of interpreted recursion costs several frames and the old default of 256 refused a recursion only a hundred deep. The default is now 2048 and the ceiling 8192, which was measured against the host stack: 16,384 frames still run, and the first overflow appears above 20,000.
- A nested interpreter was given the caller's whole frame budget while sharing the caller's host stack, so a deep chain of `interpret()` calls could overflow it. A child now inherits the caller's remaining frames, exactly as it already inherited the remaining step budget.

### Verification

- 7 CTest checks pass, in both the normal build and the AddressSanitizer/UndefinedBehaviorSanitizer build: core smoke tests, example programs, lexer/parser unit tests, arithmetic boundary tests, fuzzing, reference comparison, and the reproducible-build check.
- New direct unit tests cover tokens, literal types, escapes, source positions, parse statuses, and AST shape; new arithmetic tests pin promotion, conversion, wraparound, and boundary behavior.
- Fuzzing feeds generated C fragments and random bytes through a session and requires a diagnostic and a still-usable session, never a crash or a hang. It runs clean across many seeds under sanitizers, and builds as a libFuzzer target with `-DCT_FUZZER=ON`.
- 27 successful examples match native GCC-compiled C17 output and exit status byte for byte, now as a CTest check rather than a manual comparison. That includes the donut, which pins several thousand floating-point results at once. The maze and Life's random soup are excluded because `rand` is Cterpreter's own generator. The three diagnostic examples still produce their intended errors.
- Two copies of the source tree at different paths produce a byte-identical core archive.
- The REPL's hints, completions, and live diagnostics were exercised through a pseudo-terminal.
- The examples collection is now **30 C programs in ten feature folders**: 27 successful programs and three deliberate diagnostic failures. The three new `types` programs cover aggregates, function pointers, and the numeric tower; the six `playground` programs exist to be poked at.

### Still unfinished

- Bit-fields, `long double`, complex numbers, `_Atomic`, variable-length arrays, variadic interpreted functions, and `va_list`.
- `strtoul` and the wide-character library, noticed as missing while writing the playground.
- `volatile` and `restrict` are parsed and ignored; `extern` declares rather than references. `const` is modelled; the other qualifiers are not.
- A standalone semantic type checker. Checking still happens largely during execution, so invalid constructs in unexecuted branches can escape it.
- Accurate original-source locations through includes and macro expansion, and the rest of the C17 preprocessing rules.
- Real memory snapshots for sessions. Save and restore still replays source and repeats side effects.
- clangd/LSP integration and the virtual source file it would need; `.asm`, which needs a native backend; a debugger; signal handling; dynamically loaded libraries and native calls; and configurable undefined-behavior policies.
- Self-interpretation. `interpret()` nests interpreters, but Cterpreter still cannot run its own source, so the whole Cterpreterception section remains open.

The type system was the largest architectural gap in 0.2.0 and it is now closed. What stands between here and the acceptance test is no longer representation but coverage: the interpreter's own source uses features it does not yet implement.

## 2026-09-20 — Current state: 0.2.0

Cterpreter has grown from an echo loop into a working interpreter for a C subset. It executes its own AST, runs small programs with `main`, and preserves state in a REPL. It does not invoke a compiler to execute user code. The full roadmap, especially self-interpretation, is still unfinished.

### Working now

- A lexer, AST parser, evaluator, and preprocessor, split into separate C17 modules.
- `int`, `double`, `char`, `void`, pointers, one-dimensional arrays, scalar/pointer typedefs, enums, casts, `sizeof`, `_Alignof`, and `_Generic` within the supported type model.
- Functions and prototypes, recursion, lexical scopes, globals, static locals, loops, conditionals, switch statements, and jumps to labels in the same or an enclosing block.
- Managed object storage, pointer arithmetic, allocation/reallocation/freeing, and diagnostics for invalid access, expired storage, uninitialized reads, and arithmetic errors.
- Supported standard headers, includes, object/function-like/variadic macros, conditional preprocessing, stringification, and token concatenation. Preprocessing is not yet fully C17-compatible.
- `printf` and formatted input, string/memory helpers, common math and character functions, file I/O, command-line arguments, environment access, and separate interpreter contexts.
- A terminal REPL with multiline input, editing, history, basic highlighting and suggestions, configurable prompts/colors, cancellation, and execution limits.
- `.type`, `.ast`, `.vars`, `.source`, and source save/load/replay. Inspection uses current macros and type aliases without running the inspected expression.

### Examples added and organized

The collection now contains **21 C programs in nine feature folders**, including 18 successful programs and three deliberate diagnostic failures. It includes merge sort, N-Queens backtracking, Dijkstra's shortest paths, a bytecode stack machine, a growing vector, text processing, macro demonstrations, and file-based reports.

The original examples moved to:

- `examples/basics/hello.c`
- `examples/recursion/factorial.c`
- `examples/memory/squares.c`

The [examples index](examples/README.md) records the features, inputs, and expected results. CMake and README references use the new paths.

### Verification so far

- The project builds with strict compiler warnings on GCC/Linux.
- The focused core and example CTest checks pass, including with AddressSanitizer and UndefinedBehaviorSanitizer.
- All 18 successful examples match native GCC-compiled C17 output and exit status for the sample inputs. The diagnostic examples return status 1 with the intended errors.
- Terminal editing, Tab completion, history, Ctrl+C, EOF, and session replay have been exercised through a pseudo-terminal.
- CI is configured for GCC/Clang on Linux and Apple Clang on macOS. Hosted CI and a native macOS build have not been run from this workspace.

### Still unfinished

- Structures/unions, complex declarators, function pointers, multidimensional arrays, compound literals, designated initializers, and local typedef/enum definitions.
- The wider C numeric type system, complete integer promotions/conversions, a full semantic type checker, qualifiers, atomics, and unrestricted control flow.
- Full preprocessing, accurate original-source locations through includes and macro expansion, and the rest of the standard library.
- Real memory snapshots for sessions. Current save/restore replays source and repeats side effects.
- clangd/LSP integration, context-aware completion, signature help, live diagnostics, and a debugger. Current suggestions use a built-in vocabulary.
- Native/dynamic-library extensions, the remaining boot-time theatrics, and configurable undefined-behavior policies.
- Running Cterpreter's own source, nested interpreted instances, and the ultimate Cterpreterception acceptance test.

The largest architectural gap is the type system: the current scalar/pointer representation needs to grow to describe aggregates, declarators, and the remaining numeric types. The examples establish useful current behavior, not completion of those features.

The [roadmap](Cterpreter_Roadmap.md) remains the detailed checklist; the [README](README.md) documents how to use what exists today.
