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
#   E_PP    — g++ preprocessor rejected the source (e.g. c++11 syntax
#             under a -std=c++03 default); these are infra problems,
#             not sea-front bugs
#   E_FRONT — sea-front errored / crashed during transpile
#   E_CC    — cc errored on the emitted C
#   E_RUN   — binary segfaulted / killed by signal
#   SKIP    — not a 'dg-do run' test, or filtered out
#
# Per-test dg-options are parsed from the source file. Currently we
# extract the -std=... flag (so cpp0x/* tests are preprocessed as
# C++11 instead of the c++03 default in sea-front-cc); other dg-options
# are ignored.
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
n_epp=0
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

    # Parse the test's dg-options for a -std= flag and feed it through
    # to sea-front-cc (which forwards it to the preprocessor). Without
    # this every cpp0x/* test fails at the g++ preprocess step because
    # sea-front-cc defaults to -std=c++03. The regex grabs the FIRST
    # -std=... token in any dg-options "..." directive in the file.
    test_std=$(grep -oE 'dg-options[[:space:]]*"[^"]*-std=[^[:space:]"]+' "$src" \
               2>/dev/null | head -1 | grep -oE -- '-std=[^[:space:]"]+')
    extra_flags=""
    if [ -n "$test_std" ]; then
        extra_flags="$test_std"
    fi

    # Step 1: transpile + compile + link via sea-front-cc.
    # -lstdc++ for the C++ runtime (RTTI vtables, operator new/delete,
    # std::__throw_* helpers) since the emitted C still references the
    # mangled C++ names. -lm for math/abort dependents some tests pull in.
    if ! timeout "$SEA_DG_TIMEOUT" "$SEA_FRONT_CC" -O0 -w $extra_flags "$src" -o "$bin" \
            -lstdc++ -lm \
            > "$WORK/out" 2> "$WORK/err"; then
        # Bucket the failure. The preprocess step errors with a path
        # under the input testsuite tree (not /tmp/sf_pp_*.i); sea-front
        # errors point at the preprocessed .i file; cc errors point at
        # the emitted .c (sf_out_*.c) or the original source.
        if grep -qE '/sf_pp_.*\.i:[0-9]+' "$WORK/err" 2>/dev/null; then
            echo "E_FRONT  $rel"
            n_efront=$((n_efront + 1))
        elif grep -qE '/sf_out_.*\.c:[0-9]+' "$WORK/err" 2>/dev/null; then
            echo "E_CC     $rel"
            n_ecc=$((n_ecc + 1))
        elif grep -qE 'error:' "$WORK/err" 2>/dev/null; then
            # Error from the preprocess step (g++ -E) — the source path
            # appears in the message but neither /sf_pp_ nor /sf_out_.
            echo "E_PP     $rel"
            n_epp=$((n_epp + 1))
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
echo "  e_pp:    $n_epp"
echo "  e_front: $n_efront"
echo "  e_cc:    $n_ecc"
echo "  e_run:   $n_erun"
