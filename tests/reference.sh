#!/bin/sh
# Compare every example's interpreted behaviour with the same program compiled
# by the host C compiler. Skips silently when no reference compiler is present.
set -u
root=${1:-.}
interpreter=${CTERPRETER:-$root/build/Cterpreter}
compiler=${CC:-cc}
if ! "$compiler" --version >/dev/null 2>&1; then
    echo "no reference compiler; skipping"
    exit 0
fi
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
failures=0

compare() {
    source=$root/$1
    shift
    input=/dev/null
    if [ "${1:-}" = "--stdin" ]; then input=$2; shift 2; fi
    if ! "$compiler" -std=c17 -w -o "$work/native" "$source" -lm 2>"$work/compile.log"; then
        echo "skip $source (the reference compiler rejected it)"
        return
    fi
    "$work/native" "$@" <"$input" >"$work/native.out" 2>&1
    native_status=$?
    "$interpreter" "$source" "$@" <"$input" >"$work/interpreted.out" 2>&1
    interpreted_status=$?
    if [ "$native_status" != "$interpreted_status" ] || ! cmp -s "$work/native.out" "$work/interpreted.out"; then
        echo "FAIL $source (native $native_status, interpreted $interpreted_status)"
        diff "$work/native.out" "$work/interpreted.out" | head -20
        failures=$((failures + 1))
    else
        echo "ok   $source"
    fi
}

printf '12 + 5\n9 / 0\n8 * 3\n' >"$work/calculator.in"
compare examples/basics/hello.c Ada
compare examples/recursion/factorial.c
compare examples/recursion/merge_sort.c
compare examples/recursion/mutual_recursion.c
compare examples/recursion/n_queens.c
compare examples/control_flow/state_machine.c
compare examples/control_flow/stack_machine.c
compare examples/memory/squares.c
compare examples/memory/dynamic_vector.c
compare examples/memory/shortest_path.c
compare examples/memory/sieve.c
compare examples/strings/word_frequency.c
compare examples/strings/run_length.c
compare examples/preprocessor/macros.c
compare examples/types/typedefs_generics.c
compare examples/types/aggregates.c
compare examples/types/function_pointers.c
compare examples/types/numeric_types.c
compare examples/io/csv_report.c
compare examples/io/calculator.c --stdin "$work/calculator.in"

for diagnostic in "$root"/examples/diagnostics/*.c; do
    "$interpreter" "$diagnostic" >/dev/null 2>&1
    if [ $? -ne 1 ]; then
        echo "FAIL $diagnostic was not diagnosed"
        failures=$((failures + 1))
    else
        echo "ok   $diagnostic (diagnosed)"
    fi
done

[ "$failures" -eq 0 ] || echo "$failures reference comparisons failed"
exit $failures
