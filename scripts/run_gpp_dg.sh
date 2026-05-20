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
#   XFAIL   — listed in scripts/dg-xfail.txt as out-of-scope and DID
#             fail as expected (Itanium ABI conformance tests etc. —
#             see the rationale in that file)
#   XPASS   — listed as xfail but actually PASSED. Loud — some other
#             slice probably fixed it; the xfail entry should be
#             removed.
#   SKIP    — not a 'dg-do run' test, or filtered out
#
# Per-test dg-options are parsed from the source file. Currently we
# extract the -std=... flag (so cpp0x/* tests are preprocessed as
# C++11 instead of the c++03 default in sea-front-cc); other dg-options
# are ignored.
#
# Env:
#   SEA_GCC_TESTSUITE  default: $HOME/src/sea-front-deps/gcc-4.8.5/gcc/testsuite
#   SEA_DG_XFAIL_FILE  default: scripts/dg-xfail.txt — known-bad list
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
SEA_DG_XFAIL_FILE="${SEA_DG_XFAIL_FILE:-$SCRIPT_DIR/dg-xfail.txt}"

# Load xfail list (one rel-path per line, '#' comments). Joined into
# a newline-delimited string; matches are exact rel-path equality.
XFAIL_LIST=""
if [ -f "$SEA_DG_XFAIL_FILE" ]; then
    XFAIL_LIST=$(grep -vE '^\s*(#|$)' "$SEA_DG_XFAIL_FILE" \
                  | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
fi
is_xfail() {
    case "
$XFAIL_LIST
" in
        *"
$1
"*) return 0 ;;
    esac
    return 1
}

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
n_xfail=0
n_xpass=0

# emit_bucket prints the outcome and updates the counters, intercepting
# xfailed tests: non-PASS → XFAIL, PASS → XPASS. The intercepted bucket
# names are preserved on the printed line so the cause is still visible.
emit_bucket() {
    bucket=$1   # PASS, FAIL, E_PP, E_FRONT, E_CC, E_RUN
    rel=$2
    detail=${3:-}
    if is_xfail "$rel"; then
        if [ "$bucket" = PASS ]; then
            echo "XPASS    $rel"
            n_xpass=$((n_xpass + 1))
        else
            # Strip outer parens from detail so the composite tag
            # reads e.g. '(FAIL exit 3)' not '(FAIL (exit 3))'.
            inner=$(printf %s "$detail" | sed -e 's/^(//' -e 's/)$//')
            echo "XFAIL    $rel ($bucket${inner:+ $inner})"
            n_xfail=$((n_xfail + 1))
        fi
        return
    fi
    if [ -n "$detail" ]; then
        printf '%-8s %s %s\n' "$bucket" "$rel" "$detail"
    else
        printf '%-8s %s\n' "$bucket" "$rel"
    fi
    case "$bucket" in
        PASS)    n_pass=$((n_pass + 1)) ;;
        FAIL)    n_fail=$((n_fail + 1)) ;;
        E_PP)    n_epp=$((n_epp + 1)) ;;
        E_FRONT) n_efront=$((n_efront + 1)) ;;
        E_CC)    n_ecc=$((n_ecc + 1)) ;;
        E_RUN)   n_erun=$((n_erun + 1)) ;;
    esac
}

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
        #
        # Link errors look like '/usr/bin/ld: ... undefined reference to'
        # OR 'collect2: error: ld returned ...' — bucket as E_CC so they
        # don't pollute the preprocessor-error count (the failure is in
        # symbol resolution, downstream of sea-front transpile and cc
        # compile). Note: this lumps link errors with cc errors; if a
        # separate E_LINK bucket becomes useful, split here.
        if grep -qE '/sf_pp_.*\.i:[0-9]+' "$WORK/err" 2>/dev/null; then
            emit_bucket E_FRONT "$rel"
        elif grep -qE '/sf_out_.*\.c:[0-9]+' "$WORK/err" 2>/dev/null; then
            emit_bucket E_CC "$rel"
        elif grep -qE '(undefined reference|ld returned|/usr/bin/ld:)' \
                 "$WORK/err" 2>/dev/null; then
            emit_bucket E_CC "$rel"
        elif grep -qE 'error:' "$WORK/err" 2>/dev/null; then
            # Error from the preprocess step (g++ -E) — the source path
            # appears in the message but neither /sf_pp_ nor /sf_out_.
            emit_bucket E_PP "$rel"
        else
            emit_bucket E_FRONT "$rel"
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
        emit_bucket PASS "$rel"
    elif [ "$rc" -ge 128 ]; then
        emit_bucket E_RUN "$rel" "(signal $((rc - 128)))"
    else
        emit_bucket FAIL "$rel" "(exit $rc)"
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
echo "  xfail:   $n_xfail"
if [ "$n_xpass" -gt 0 ]; then
    echo "  xpass:   $n_xpass  (these were listed as xfail but PASSED;"
    echo "                       update $SEA_DG_XFAIL_FILE)"
fi
