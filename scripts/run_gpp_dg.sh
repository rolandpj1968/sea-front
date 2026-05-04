#!/bin/sh
# run_gpp_dg.sh — drive sea-front-cc against gcc's g++.dg testsuite.
#
# Picks every .C file annotated 'dg-do run' under SEA_GCC_TESTSUITE
# (default: gcc-4.8 g++.dg), transpiles + compiles + runs each, and
# reports PASS/FAIL based on exit code. The dg convention is:
#   exit 0          → test passed
#   abort() / non-0 → test failed
#
# Output: one line per test (PASS/FAIL/ERROR/SKIP) + summary at end.
# Buckets:
#   PASS    — sea-front transpiled, cc compiled, binary ran with exit 0
#   FAIL    — binary ran with non-zero exit (test's own failure path)
#   E_FRONT — sea-front errored / crashed during transpile
#   E_CC    — cc errored on the emitted C
#   E_RUN   — binary segfaulted / killed by signal
#   SKIP    — not a 'dg-do run' test, or filtered out
#
# Env:
#   SEA_GCC_TESTSUITE  default: $HOME/src/sea-front-deps/gcc-4.8.5/gcc/testsuite
#   SEA_DG_LIMIT       cap on number of tests (default: all)
#   SEA_DG_FILTER      grep -E pattern on relative path (default: all)
#   SEA_DG_TIMEOUT     per-test wall-clock seconds (default: 10)
#   SEA_DG_VERBOSE     1 to print every test path
#
# Defaults assume sea-front's project layout:
#   build/sea-front  is the transpiler binary
#   scripts/sea-front-cc  is the cc shim

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJ_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

SEA_FRONT="${SEA_FRONT:-$PROJ_ROOT/build/sea-front}"
SEA_FRONT_CC="${SEA_FRONT_CC:-$SCRIPT_DIR/sea-front-cc}"
SEA_GCC_TESTSUITE="${SEA_GCC_TESTSUITE:-$HOME/src/sea-front-deps/gcc-4.8.5/gcc/testsuite}"
SEA_DG_LIMIT="${SEA_DG_LIMIT:-0}"
SEA_DG_FILTER="${SEA_DG_FILTER:-}"
SEA_DG_TIMEOUT="${SEA_DG_TIMEOUT:-10}"
SEA_DG_VERBOSE="${SEA_DG_VERBOSE:-0}"

if [ ! -x "$SEA_FRONT" ]; then
    echo "ERROR: sea-front not built — run 'make' first ($SEA_FRONT)" >&2
    exit 2
fi
if [ ! -d "$SEA_GCC_TESTSUITE/g++.dg" ]; then
    echo "ERROR: testsuite not found at $SEA_GCC_TESTSUITE/g++.dg" >&2
    exit 2
fi

export SEA_FRONT
export SEA_LIBSTDCXX_SRC="${SEA_LIBSTDCXX_SRC:-}"
export SEA_LIBSTDCXX_BUILD="${SEA_LIBSTDCXX_BUILD:-}"

TMPDIR=${TMPDIR:-/tmp}
WORK=$TMPDIR/sea-front-dg-$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

n_total=0
n_pass=0
n_fail=0
n_efront=0
n_ecc=0
n_erun=0

# Collect test paths.
files=$(grep -rlE 'dg-do[[:space:]]+run' "$SEA_GCC_TESTSUITE/g++.dg" 2>/dev/null \
        | sort)

if [ -n "$SEA_DG_FILTER" ]; then
    files=$(echo "$files" | grep -E "$SEA_DG_FILTER")
fi
if [ "$SEA_DG_LIMIT" != 0 ]; then
    files=$(echo "$files" | head -n "$SEA_DG_LIMIT")
fi

for src in $files; do
    n_total=$((n_total + 1))
    rel=${src#$SEA_GCC_TESTSUITE/}
    bin=$WORK/t$n_total

    [ "$SEA_DG_VERBOSE" = "1" ] && echo "RUN  $rel" >&2

    # Step 1: transpile + compile + link via sea-front-cc.
    # -lstdc++ for the C++ runtime (RTTI vtables, operator new/delete,
    # std::__throw_* helpers) since the emitted C still references the
    # mangled C++ names. -lm for math/abort dependents some tests pull in.
    if ! timeout "$SEA_DG_TIMEOUT" "$SEA_FRONT_CC" -O0 -w "$src" -o "$bin" \
            -lstdc++ -lm \
            > "$WORK/out" 2> "$WORK/err"; then
        # Distinguish sea-front error from cc error by scanning stderr.
        if grep -qE 'error:' "$WORK/err" 2>/dev/null; then
            if grep -qE '/sf_pp_.*\.i:[0-9]+' "$WORK/err" 2>/dev/null; then
                echo "E_FRONT  $rel"
                n_efront=$((n_efront + 1))
            else
                echo "E_CC     $rel"
                n_ecc=$((n_ecc + 1))
            fi
        else
            echo "E_FRONT  $rel"
            n_efront=$((n_efront + 1))
        fi
        continue
    fi

    # Step 2: run.
    if timeout "$SEA_DG_TIMEOUT" "$bin" > /dev/null 2>&1; then
        rc=0
    else
        rc=$?
    fi

    if [ "$rc" = 0 ]; then
        echo "PASS     $rel"
        n_pass=$((n_pass + 1))
    elif [ "$rc" -ge 128 ]; then
        echo "E_RUN    $rel (signal $((rc - 128)))"
        n_erun=$((n_erun + 1))
    else
        echo "FAIL     $rel (exit $rc)"
        n_fail=$((n_fail + 1))
    fi
done

echo
echo "=== summary ==="
echo "  total:   $n_total"
echo "  pass:    $n_pass"
echo "  fail:    $n_fail"
echo "  e_front: $n_efront"
echo "  e_cc:    $n_ecc"
echo "  e_run:   $n_erun"
