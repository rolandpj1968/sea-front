#!/bin/sh
# End-to-end emit-c test against the cproc/QBE back-end:
#   1. scripts/cproc++  (g++ -E → sea-front → cproc → qbe → as → ld)
#   2. run executable   → exit code matches expected
#
# Mirror of test_emit_c.sh that swaps the host-cc back-end for
# cproc/QBE via the cproc++ wrapper. Same fixtures, same EXPECT
# convention. Most tests are expected to pass; the few that don't
# are useful signal — either sea-front emits something cproc rejects
# (sea-front gap) or cproc upstream doesn't implement the construct
# (cproc/QBE gap). The script doesn't gate on PASS/FAIL counts;
# the summary line is the deliverable.
#
# Usage: test_emit_c_cproc.sh [sea-front-binary]
# Env:
#   SEA_CPROC     path to cproc binary  (default: ../cproc/cproc)
#   SEA_QBE_DIR   dir containing qbe    (default: ../qbe)

SEA_FRONT="${1:-build/sea-front}"
TESTDIR="$(dirname "$0")/emit_c"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN_DIR="$REPO_ROOT/gen/emit_c_cproc"
rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"
TMPDIR="$GEN_DIR"

# cproc / qbe defaults — adjacent checkouts under ~/src.
SEA_CPROC="${SEA_CPROC:-$REPO_ROOT/../cproc/cproc}"
SEA_QBE_DIR="${SEA_QBE_DIR:-$REPO_ROOT/../qbe}"
export SEA_CPROC SEA_QBE_DIR
export SEA_FRONT

if [ ! -x "$SEA_CPROC" ]; then
    echo "SKIP: cproc not found at $SEA_CPROC (set SEA_CPROC)"
    exit 0
fi
if [ ! -x "$SEA_QBE_DIR/qbe" ]; then
    echo "SKIP: qbe not found at $SEA_QBE_DIR/qbe (set SEA_QBE_DIR)"
    exit 0
fi

CPROCXX="$REPO_ROOT/scripts/cproc++"

PASS=0
EMIT_FAIL=0   # sea-front --emit-c failed (shouldn't happen — gated by test_emit_c)
CC_FAIL=0     # cproc/qbe rejected the emitted C
RUN_FAIL=0    # binary built but wrong exit code

FAIL_LOG="$TMPDIR/_failures.log"
: > "$FAIL_LOG"

for cpp in "$TESTDIR"/*.cpp; do
    [ -f "$cpp" ] || continue
    name="$(basename "$cpp" .cpp)"

    expected="$(head -1 "$cpp" | sed -n 's|^// EXPECT: \([0-9]*\)|\1|p')"
    if [ -z "$expected" ]; then
        continue
    fi

    bin="$TMPDIR/$name"
    err="$TMPDIR/$name.err"

    if ! "$CPROCXX" "$cpp" -o "$bin" >"$err" 2>&1; then
        # Distinguish sea-front emit-c failure from cproc compile failure
        # by re-running just the emit-c step.
        if ! "$SEA_FRONT" --emit-c "$cpp" >/dev/null 2>&1; then
            EMIT_FAIL=$((EMIT_FAIL + 1))
            echo "EMIT_FAIL $name" >> "$FAIL_LOG"
        else
            CC_FAIL=$((CC_FAIL + 1))
            echo "CC_FAIL $name" >> "$FAIL_LOG"
        fi
        continue
    fi

    actual=0
    "$bin" >/dev/null 2>&1 || actual=$?
    if [ "$actual" = "$expected" ]; then
        PASS=$((PASS + 1))
    else
        RUN_FAIL=$((RUN_FAIL + 1))
        echo "RUN_FAIL $name (exit $actual, expected $expected)" >> "$FAIL_LOG"
    fi
done

TOTAL=$((PASS + EMIT_FAIL + CC_FAIL + RUN_FAIL))
echo ""
echo "cproc/qbe emit-c: $PASS / $TOTAL passed"
echo "  emit-c failures: $EMIT_FAIL  cproc-compile failures: $CC_FAIL  run failures: $RUN_FAIL"
echo "  see $FAIL_LOG for the per-test failure breakdown"
