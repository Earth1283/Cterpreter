# Performance comparisons

`run.py` races Cterpreter builds against CPython and Node.js using C programs
and their ports in `workloads.py` and `workloads.js`. It first compiles each C
program with the host compiler and checks every run's stdout, in every
language, against that native result.
Unexpected diagnostics or nonzero exit status fail the run. The kernels cover
recursive and leaf calls, integer and floating arithmetic, arrays, sorting,
switches and string/ctype builtins.

Use independently built Release binaries. Keep the baseline source in a
separate checkout so rebuilding it cannot accidentally incorporate the changes:

```sh
git worktree add --detach /tmp/cterpreter-before b56cba8
cmake -S /tmp/cterpreter-before -B build/bench-before -DCMAKE_BUILD_TYPE=Release
cmake --build build/bench-before -j 4
cmake -S . -B build/bench-after -DCMAKE_BUILD_TYPE=Release
cmake --build build/bench-after -j 4
python3 benchmarks/run.py \
  --engine before=build/bench-before/Cterpreter \
  --engine after=build/bench-after/Cterpreter \
  --repeats 15 --output build/benchmark-results.json
```

The long suite (`--suite long`, or `--suite all`) holds eleven kernels that run
for 0.2-1.5 s each, so startup is a small fraction of the time: `fib(35)`, an
8M sieve, 11-queens, Collatz chains, quicksort, 120×120 matrix multiply, LCS
dynamic programming, a larger Mandelbrot, spectral norm (double-returning
calls), a 300k-iteration string loop and a 32-bit hash loop. It needs a higher
step limit and timeout:

```sh
python3 benchmarks/run.py --engine after=build/bench-after/Cterpreter \
  --suite long --repeats 9 --max-steps 100000000000 --timeout 600
```

Python 3.10 or newer is needed for the switch port's `match` statement. A C
compiler is required for output validation. `--python` selects another Python
executable; `--node` selects the Node.js used for the JavaScript ports, which
run whenever `node` is on the `PATH` unless `--no-node` is given; `--cc`
selects the native reference compiler. `--case` can be
repeated to run a subset. On Linux the runner pins itself and its child
processes to the first allowed CPU; `--cpu` selects a different allowed CPU.

Each engine gets one untimed warmup process per workload, then fresh processes
are run in a seeded, shuffled order for every repetition. Reported times are
medians of wall time, including process startup, loading/parsing and output.
Warmups populate OS caches; Python's runtime specialization still happens in
each timed process. Cterpreter's configuration and history are disabled and
its execution limit is raised equally for all kernels. Python uses local
variables inside functions and the same algorithms and sizes; it does not
replace loops with NumPy or library sorting. Arrays use Python lists or a
bytearray, so object representations and runtime checks naturally differ. The
JavaScript ports keep the same algorithms with typed arrays for numeric arrays
and `Math.imul`/`>>> 0` for 32-bit unsigned arithmetic; V8's JIT warms up
inside each timed process, and Node's startup (about 30 ms here) is included.

The long suite's limit of 100 billion steps lets Cterpreter compile to native
code under `--jit auto`. Pass `--jit off` to an engine's command line, by
building a wrapper script, to time the interpreter alone.

The JSON contains every timing, checksums, compiler/Python/Node versions, CPU
affinity, and SHA-256 hashes of the binaries and workload sources. The suite's
total is the sum of the per-workload medians, not a general language speed
rating. Short workloads are particularly sensitive to startup time and small
differences can be noise on a shared machine.

For semantic comparisons against the baseline, including diagnostics, strict
mode and low step/depth limits:

```sh
python3 tests/differential.py \
  build/bench-before/Cterpreter build/bench-after/Cterpreter --count 2100
ctest --test-dir build/bench-after --output-on-failure
```

The dated [results](results/2026-11-06.md) record the native compiler; the
[previous pass](results/2026-10-04.md) records interpreter optimizations.
