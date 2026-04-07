#!/bin/sh
# End-to-end emit-c test:
#   1. sea-front --emit-c <input.cpp>  → C source
#   2. cc -o <bin> <c source>          → executable
#   3. run executable                  → exit code matches expected
#
# Usage: test_emit_c.sh [sea-front-binary]

set -e

SEA_FRONT="${1:-build/sea-front}"
TESTDIR="$(dirname "$0")/emit_c"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

PASS=0
FAIL=0

for cpp in "$TESTDIR"/*.cpp; do
    [ -f "$cpp" ] || continue
    name="$(basename "$cpp" .cpp)"

    # Expected exit code lives on the first line as: // EXPECT: <n>
    expected="$(head -1 "$cpp" | sed -n 's|^// EXPECT: \([0-9]*\)|\1|p')"
    if [ -z "$expected" ]; then
        echo "SKIP $name (no // EXPECT: <n>)"
        continue
    fi

    if ! "$SEA_FRONT" --emit-c "$cpp" > "$TMPDIR/$name.c" 2>"$TMPDIR/$name.err"; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name (emit-c failed)"
        cat "$TMPDIR/$name.err"
        continue
    fi

    if ! cc -o "$TMPDIR/$name" "$TMPDIR/$name.c" 2>"$TMPDIR/$name.cc.err"; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name (cc failed)"
        cat "$TMPDIR/$name.cc.err"
        continue
    fi

    actual=0
    "$TMPDIR/$name" || actual=$?
    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name (exit $actual, expected $expected)"
    fi
done

echo ""
echo "emit-c tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
