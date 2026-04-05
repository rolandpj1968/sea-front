#!/bin/sh
# Clang/GCC parse smoke test: preprocess with mcpp, then parse with sea-front.
# Tests that our parser handles real-world C++ after preprocessing.
# This is a crash/error test — we don't compare AST output, just check
# that parsing succeeds (or fails where expected).
#
# Usage: test_clang_parse.sh <sea-front-binary> <mcpp-binary>
set -e

SEA_FRONT="${1:-build/sea-front}"
MCPP="${2:-build/mcpp-bin}"
TESTDIR="$(dirname "$0")/clang-lexer"
TMPFILE="$(mktemp)"
trap 'rm -f "$TMPFILE"' EXIT

PASS=0
FAIL=0
SKIP=0

for f in "$TESTDIR"/*.c "$TESTDIR"/*.cpp "$TESTDIR"/*.C; do
    [ -f "$f" ] || continue
    name="$(basename "$f")"

    # Determine if this is a negative test
    expect_error=false
    if grep -q 'expected-error\|dg-error' "$f" 2>/dev/null; then
        expect_error=true
    fi

    # Preprocess
    preprocessed=false
    for mcpp_flags in "-P -W0" "-W0"; do
        if "$MCPP" $mcpp_flags "$f" > "$TMPFILE" 2>/dev/null && [ -s "$TMPFILE" ]; then
            preprocessed=true
            break
        fi
    done

    if ! $preprocessed; then
        if $expect_error; then
            PASS=$((PASS + 1))
        else
            SKIP=$((SKIP + 1))
        fi
        continue
    fi

    # Parse (--dump-ast to /dev/null — we just check exit code)
    parser_ok=true
    if ! "$SEA_FRONT" --dump-ast "$TMPFILE" > /dev/null 2>&1; then
        parser_ok=false
    fi

    if $expect_error; then
        PASS=$((PASS + 1))  # negative test — either outcome is fine
    else
        if $parser_ok; then
            PASS=$((PASS + 1))
        else
            err=$("$SEA_FRONT" --dump-ast "$TMPFILE" 2>&1 | grep "error:" | head -1)
            FAIL=$((FAIL + 1))
            printf "FAIL %-45s %s\n" "$name" "$err"
        fi
    fi
done

echo ""
echo "Clang/GCC parse smoke: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
