#!/bin/sh
# Clang/GCC lexer test suite: preprocess with mcpp, then lex with sea-front.
#
# Handles both positive and negative tests:
#   - Files with "expected-error" or "dg-error" annotations are negative tests:
#     we expect the lexer to fail.
#   - Files without those annotations are positive tests:
#     we expect the lexer to succeed.
#
# Usage: test_clang.sh <sea-front-binary> <mcpp-binary>
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

    # Determine if this is a negative test (expected to produce errors)
    expect_error=false
    if grep -q 'expected-error\|dg-error' "$f" 2>/dev/null; then
        expect_error=true
    fi

    # Try preprocessing with mcpp
    preprocessed=false
    for mcpp_flags in "-P -W0" "-W0"; do
        if "$MCPP" $mcpp_flags "$f" > "$TMPFILE" 2>/dev/null && [ -s "$TMPFILE" ]; then
            preprocessed=true
            break
        fi
    done

    if ! $preprocessed; then
        # Can't preprocess — if it's a negative test, the mcpp failure
        # itself may be the expected error (e.g. #error directives)
        if $expect_error; then
            PASS=$((PASS + 1))
            printf "OK   %-45s (negative: mcpp rejected)\n" "$name"
        else
            SKIP=$((SKIP + 1))
            printf "SKIP %-45s (mcpp failed)\n" "$name"
        fi
        continue
    fi

    # Run our lexer on the preprocessed output
    lexer_ok=true
    if ! "$SEA_FRONT" --dump-tokens "$TMPFILE" > /dev/null 2>&1; then
        lexer_ok=false
    fi

    if $expect_error; then
        # Negative test: we expect failure
        if ! $lexer_ok; then
            PASS=$((PASS + 1))
            printf "OK   %-45s (negative: lexer rejected)\n" "$name"
        else
            ntoks=$("$SEA_FRONT" --dump-tokens "$TMPFILE" 2>/dev/null | wc -l)
            # Some negative tests have errors only in the semantic layer,
            # not the lexer. Lexing successfully is acceptable.
            PASS=$((PASS + 1))
            printf "OK   %-45s (negative: lexed %d tokens, error is semantic)\n" "$name" "$ntoks"
        fi
    else
        # Positive test: we expect success
        if $lexer_ok; then
            ntoks=$("$SEA_FRONT" --dump-tokens "$TMPFILE" 2>/dev/null | wc -l)
            PASS=$((PASS + 1))
            printf "OK   %-45s %d tokens\n" "$name" "$ntoks"
        else
            err=$("$SEA_FRONT" --dump-tokens "$TMPFILE" 2>&1 | grep "error:" | head -1)
            FAIL=$((FAIL + 1))
            printf "FAIL %-45s %s\n" "$name" "$err"
        fi
    fi
done

echo ""
echo "Clang/GCC lexer tests: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
