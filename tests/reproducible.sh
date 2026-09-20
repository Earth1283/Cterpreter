#!/bin/sh
# Build the core library twice from two different source paths and compare the
# results. Identical archives mean no absolute path or timestamp leaked in.
set -u
root=${1:-.}
generator=${2:-}
command -v cmake >/dev/null 2>&1 || { echo "no cmake; skipping"; exit 0; }
work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT
for name in first second; do
    mkdir -p "$work/$name"
    (cd "$root" && tar cf - --exclude=build --exclude=.git .) | (cd "$work/$name" && tar xf -) || exit 1
    if [ -n "$generator" ]; then
        cmake -S "$work/$name" -B "$work/$name/out" -G "$generator" -DBUILD_TESTING=OFF >/dev/null || exit 1
    else
        cmake -S "$work/$name" -B "$work/$name/out" -DBUILD_TESTING=OFF >/dev/null || exit 1
    fi
    cmake --build "$work/$name/out" --target cterpreter_core >/dev/null || exit 1
done
first=$(find "$work/first/out" -name 'libcterpreter_core.a' | head -1)
second=$(find "$work/second/out" -name 'libcterpreter_core.a' | head -1)
if [ -z "$first" ] || [ -z "$second" ]; then
    echo "the library was not produced"
    exit 1
fi
if cmp -s "$first" "$second"; then
    echo "reproducible: two source paths produced an identical library"
    exit 0
fi
echo "not reproducible: the two builds differ"
exit 1
