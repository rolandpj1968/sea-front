#!/bin/sh
# Parser integration tests: run --dump-ast on each .cpp file, diff against .expected
set -e

SEA_FRONT="${1:-build/sea-front}"
TESTDIR="$(dirname "$0")/parse"
TMPFILE="$(mktemp)"
trap 'rm -f "$TMPFILE"' EXIT
PASS=0
FAIL=0

for expected in "$TESTDIR"/*.expected; do
    [ -f "$expected" ] || continue
    base="$(echo "$expected" | sed 's/\.expected$//')"
    cpp="$base.cpp"
    name="$(basename "$base")"

    if [ ! -f "$cpp" ]; then
        echo "SKIP $name (no .cpp file)"
        continue
    fi

    "$SEA_FRONT" --dump-ast "$cpp" > "$TMPFILE" 2>&1 || true

    if diff -u "$expected" "$TMPFILE" > /dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name"
        diff -u "$expected" "$TMPFILE" || true
    fi
done

echo ""
echo "Parser integration tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
