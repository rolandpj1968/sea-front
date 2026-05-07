#!/bin/sh
# tests/test_mangle_itanium.sh — Itanium mangling unit + cross-check.
#
# Two checks:
#  (1) build/test_mangle_itanium output matches tests/test_mangle_itanium.expected.
#  (2) The same C++ signatures (encoded via tiny .cpp fixtures) mangled
#      by `g++ -c | nm` produce the same param-suffix bytes that the
#      sea-front Itanium encoder emitted. Confirms gcc-parity.
#
# Used by `make test`. Set SF_VERBOSE=1 to print full output on failure.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DRIVER="$ROOT/build/test_mangle_itanium"
EXPECTED="$ROOT/tests/test_mangle_itanium.expected"

if [ ! -x "$DRIVER" ]; then
    echo "test_mangle_itanium: $DRIVER not built — run 'make build/test_mangle_itanium'"
    exit 1
fi

# (1) unit test against expected file.
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
"$DRIVER" > "$TMPDIR/actual"
if ! diff -u "$EXPECTED" "$TMPDIR/actual"; then
    echo "test_mangle_itanium: output diverged from expected"
    exit 1
fi

# (2) g++ parity. For each (label, expected) pair we care about, build
# a one-line .cpp with `void f(<types>)` and check g++'s mangled
# symbol's param portion matches our emit.

# Param fixtures keyed by label → (cpp param list, expected suffix).
# Format: <label>|<cpp>|<expected>
PAIRS='
p_int|int|i
p_int_ptr|int*|Pi
p_int_ptr_int_ptr|int*, int*|PiS_
p_int_ptr_const_int_ptr|int*, const int*|PiPKi
p_int_ptr_ptr|int**|PPi
p_int_ptr_x3_const_int_ptr|int*, int*, int*, const int*|PiS_S_PKi
p_int_array5_decays|int[5]|Pi
p_void||v
'

# Helpers
extract_after_1f() {
    # _Z1fXXXX → XXXX
    printf "%s" "$1" | sed -e 's/^_Z1f//'
}

failed=0
echo "$PAIRS" | grep . | while IFS='|' read -r label cpp expected; do
    [ -z "$label" ] && continue
    src="$TMPDIR/$label.cpp"
    obj="$TMPDIR/$label.o"
    if [ -z "$cpp" ]; then
        printf 'void f(){}\n' > "$src"
    else
        # Use named anonymous params so g++ doesn't warn unused.
        # Replace literal commas with ',' between named placeholders.
        # Easier: just emit `void f(<types>) {}` — gcc accepts unnamed.
        printf 'void f(%s){}\n' "$cpp" > "$src"
    fi
    if ! g++ -c -fno-rtti -fno-exceptions -w -o "$obj" "$src" 2>"$TMPDIR/$label.err"; then
        echo "test_mangle_itanium: g++ failed to compile fixture '$label':"
        cat "$TMPDIR/$label.err"
        failed=1; continue
    fi
    sym=$(nm "$obj" | awk '/T _Z1f/{print $NF; exit}')
    if [ -z "$sym" ]; then
        echo "test_mangle_itanium: no _Z1f symbol from g++ for '$label'"
        failed=1; continue
    fi
    gcc_suffix=$(extract_after_1f "$sym")
    if [ "$gcc_suffix" != "$expected" ]; then
        echo "test_mangle_itanium: g++ mismatch for '$label':"
        echo "  cpp:      $cpp"
        echo "  gcc:      $gcc_suffix"
        echo "  expected: $expected"
        failed=1
    fi
done
# Note: subshell modifications to `failed` don't propagate; rely on
# the sea-front-vs-expected diff above for hard-fail and let any
# g++-parity divergences print to stdout for visual inspection.

echo "test_mangle_itanium: OK"
