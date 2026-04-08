#!/bin/sh
# Multi-TU link test for sea-front. Each test is a directory of
# .cpp files (e.g. tests/multi_tu/foo_*.cpp) that get
# transpiled separately, compiled to .o, linked together, and
# the resulting binary is run. The expected exit code lives on
# the first line of the *_main.cpp file as: // EXPECT: <n>
#
# This exercises the __SF_INLINE weak-symbol mechanism — without
# it, two TUs that both lower the same class would emit colliding
# Class_ctor/Class_dtor symbols and the link would fail.

set -e

SEA_FRONT="${1:-build/sea-front}"
TESTDIR="$(dirname "$0")/multi_tu"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN_DIR="$REPO_ROOT/gen/multi_tu"
rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"

PASS=0
FAIL=0

# Each "test group" is identified by a *_main.cpp file. The
# other .cpp files in the same prefix participate.
for main_cpp in "$TESTDIR"/*_main.cpp; do
    [ -f "$main_cpp" ] || continue
    name="$(basename "$main_cpp" _main.cpp)"
    expected="$(head -1 "$main_cpp" | sed -n 's|^// EXPECT: \([0-9]*\)|\1|p')"
    if [ -z "$expected" ]; then
        echo "SKIP $name (no // EXPECT: <n> on _main.cpp)"
        continue
    fi

    # Find all .cpp files belonging to this group: name_*.cpp
    group=""
    for cpp in "$TESTDIR"/${name}_*.cpp; do
        [ -f "$cpp" ] || continue
        group="$group $cpp"
    done

    # Transpile each .cpp to .c, then compile to .o
    objs=""
    failed=""
    for cpp in $group; do
        base="$(basename "$cpp" .cpp)"
        c_out="$GEN_DIR/$base.c"
        o_out="$GEN_DIR/$base.o"
        if ! "$SEA_FRONT" --emit-c "$cpp" > "$c_out" 2>"$GEN_DIR/$base.err"; then
            failed="$base (emit-c)"
            break
        fi
        if ! cc -c -o "$o_out" "$c_out" 2>"$GEN_DIR/$base.cc.err"; then
            failed="$base (cc)"
            break
        fi
        objs="$objs $o_out"
    done

    if [ -n "$failed" ]; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name ($failed)"
        cat "$GEN_DIR"/*.err 2>/dev/null
        cat "$GEN_DIR"/*.cc.err 2>/dev/null
        continue
    fi

    # Link
    bin="$GEN_DIR/$name"
    if ! cc -o "$bin" $objs 2>"$GEN_DIR/$name.link.err"; then
        FAIL=$((FAIL + 1))
        echo "FAIL $name (link)"
        cat "$GEN_DIR/$name.link.err"
        continue
    fi

    actual=0
    "$bin" || actual=$?
    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name (exit $actual, expected $expected)"
    fi
done

echo ""
echo "multi-tu tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
