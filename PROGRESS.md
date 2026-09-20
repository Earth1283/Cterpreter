# Misendeavor log

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
