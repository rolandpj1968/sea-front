#!/bin/sh
# Compile-only check for the gcc 4.8 standalone snapshots under
# tests/gcc-4.8/. Each .cpp there is a self-contained reduction of
# a real gcc 4.8 source file (vec.h, hash-table.h, etc.) with the
# system includes / GTY / mem-stat macros stubbed inline so the
# unit can be preprocessed without the full gcc tree.
#
# These don't have a 'main' — they're library-style. We just verify
# that:
#   1. mcpp pre-processes them cleanly
#   2. sea-front --emit-c produces a .c with no front-end errors
#   3. cc -c compiles the .c to .o without errors
#
# Usage: test_gcc_standalone.sh [sea-front-binary] [mcpp-binary]
#
# Persists gen/gcc_standalone/<name>.{cpp,c,o} for inspection.

set -e

SEA_FRONT="${1:-build/sea-front}"
MCPP="${2:-build/mcpp-bin}"
TESTDIR="$(dirname "$0")/gcc-4.8"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN_DIR="$REPO_ROOT/gen/gcc_standalone"
rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"

PASS=0
FAIL=0

for cpp in "$TESTDIR"/*.cpp; do
    [ -f "$cpp" ] || continue
    name="$(basename "$cpp" .cpp)"

    if ! "$MCPP" -P "$cpp" -o "$GEN_DIR/$name.cpp" 2>"$GEN_DIR/$name.mcpp.err"; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name (mcpp failed)"
        cat "$GEN_DIR/$name.mcpp.err"
        continue
    fi

    if ! "$SEA_FRONT" --emit-c "$GEN_DIR/$name.cpp" > "$GEN_DIR/$name.c" \
         2>"$GEN_DIR/$name.sf.err"; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name (sea-front --emit-c failed)"
        head -5 "$GEN_DIR/$name.sf.err"
        continue
    fi

    if ! cc -c "$GEN_DIR/$name.c" -o "$GEN_DIR/$name.o" 2>"$GEN_DIR/$name.cc.err"; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name (cc -c failed)"
        head -5 "$GEN_DIR/$name.cc.err"
        continue
    fi

    PASS=$((PASS + 1))
done

echo ""
echo "gcc standalone tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
