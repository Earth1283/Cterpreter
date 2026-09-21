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
    flags=
    while :; do
        case ${1:-} in
            --stdin) input=$2; shift 2 ;;
            --flags) flags=$2; shift 2 ;;
            *) break ;;
        esac
    done
    if ! "$compiler" -std=c17 -w -o "$work/native" "$source" -lm 2>"$work/compile.log"; then
        echo "skip $source (the reference compiler rejected it)"
        return
    fi
    "$work/native" "$@" <"$input" >"$work/native.out" 2>&1
    native_status=$?
    # shellcheck disable=SC2086
    "$interpreter" $flags "$source" "$@" <"$input" >"$work/interpreted.out" 2>&1
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
compare examples/types/variadic.c
compare examples/io/csv_report.c
compare examples/io/calculator.c --stdin "$work/calculator.in"

# The playground programs need room to run. Those that call rand() are left out:
# Cterpreter's generator is its own, so a seeded run cannot match the host's.
printf 'n\ntake semicolon\ne\nn\ntake cast\ne\nn\ne\ne\nn\ntake return\nuse return\n' >"$work/adventure.in"
big="--max-steps 900000000 --max-depth 8192"
compare examples/playground/donut.c --flags "$big" 4
compare examples/playground/life.c --flags "$big" gun 25
compare examples/playground/brainfuck.c --flags "$big" hello
compare examples/playground/brainfuck.c --flags "$big" squares
compare examples/playground/brainfuck.c --flags "$big" triangle
compare examples/playground/toolkit.c --flags "$big"
compare examples/playground/adventure.c --stdin "$work/adventure.in"

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
