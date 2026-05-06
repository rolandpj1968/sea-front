#!/bin/sh
# run_gpp_dg_via_cc1plus.sh — drive the sea-front-built cc1plus over
# g++.dg/'s dg-do-run corpus.
#
# Pipeline:
#   g++ -E -std=c++03  source.C  → preprocessed.i
#   <sea-front-built cc1plus>  -quiet -O0  preprocessed.i  → .s
#   cc -no-pie  -x assembler  .s  -lstdc++  → binary
#   binary  → exit code (0 = pass)
#
# Mirrors run_gpp_dg.sh's bucketing (PASS / FAIL / E_FRONT / E_CC / E_RUN)
# but routes through cc1plus instead of sea-front-cc, so the result
# measures how good cc1plus-built-by-sea-front is at compiling real
# C++ — independent of sea-front-cc's preprocessing layer.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

CC1PLUS="${CC1PLUS:-$HOME/src/sea-front-deps/gcc-4.8.5/build-sf/gcc/cc1plus}"
SEA_GCC_TESTSUITE="${SEA_GCC_TESTSUITE:-$HOME/src/sea-front-deps/gcc-4.8.5/gcc/testsuite}"
SEA_DG_LIMIT="${SEA_DG_LIMIT:-0}"
SEA_DG_FILTER="${SEA_DG_FILTER:-}"
SEA_DG_TIMEOUT="${SEA_DG_TIMEOUT:-15}"
SEA_DG_VERBOSE="${SEA_DG_VERBOSE:-0}"

if [ ! -x "$CC1PLUS" ]; then
    echo "ERROR: cc1plus not found at $CC1PLUS" >&2
    exit 2
fi

TMPDIR=${TMPDIR:-/tmp}
WORK=$TMPDIR/sea-front-dg-cc1plus-$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

n_total=0
n_pass=0
n_fail=0
n_efront=0
n_ecc=0
n_erun=0

files=$(grep -rlE 'dg-do[[:space:]]+run' "$SEA_GCC_TESTSUITE/g++.dg" 2>/dev/null \
        | sort)
[ -n "$SEA_DG_FILTER" ] && files=$(echo "$files" | grep -E "$SEA_DG_FILTER")
[ "$SEA_DG_LIMIT" != 0 ] && files=$(echo "$files" | head -n "$SEA_DG_LIMIT")

for src in $files; do
    n_total=$((n_total + 1))
    rel=${src#$SEA_GCC_TESTSUITE/}
    base=$WORK/t$n_total
    [ "$SEA_DG_VERBOSE" = "1" ] && echo "RUN  $rel" >&2

    # Step 1: preprocess. Point libstdc++ search path at gcc 4.8's
    # own headers (modern host libstdc++ uses C++14+ builtins like
    # __is_trivially_copyable that gcc 4.8's frontend rejects), and
    # define-out a few modern glibc attribute decorators that the 4.8
    # frontend doesn't recognise (__malloc__ with arguments, etc.).
    # The aim is to make the preprocessed input look as much like
    # gcc-4.8-era as possible, so any cc1plus failure thereafter
    # reflects a real frontend bug rather than host-environment drift.
    GCC48_LIBSTDCXX_SRC="$HOME/src/sea-front-deps/gcc-4.8.5/libstdc++-v3/include"
    GCC48_LIBSTDCXX_BUILD="$HOME/src/sea-front-deps/gcc-4.8.5/build-sf/x86_64-unknown-linux-gnu/libstdc++-v3/include"
    GCC48_LIBSUPCXX="$HOME/src/sea-front-deps/gcc-4.8.5/libstdc++-v3/libsupc++"
    GLIBC_SHIM="${SEA_GLIBC_SHIM:-$HOME/src/sea-front-deps/glibc-shim}"
    # -isystem $GLIBC_SHIM puts our era-correct sys/cdefs.h +
    # bits/floatn{,-common}.h ahead of the host's. With those in place,
    # modern glibc 2.34+ host headers reference only macros that resolve
    # cleanly under gcc 4.8 (post-4.8 attribute decorators all expand to
    # empty; __HAVE_FLOAT* are 0 so no _FloatN types are declared).
    PP_INC="-nostdinc++ \
        -I$GCC48_LIBSTDCXX_BUILD/x86_64-unknown-linux-gnu \
        -I$GCC48_LIBSTDCXX_BUILD \
        -I$GCC48_LIBSTDCXX_SRC \
        -I$GCC48_LIBSTDCXX_SRC/c_global \
        -I$GCC48_LIBSUPCXX \
        -isystem $GLIBC_SHIM"
    if ! timeout "$SEA_DG_TIMEOUT" g++ -E -std=c++03 -w \
            $PP_INC \
            "$src" -o "$base.i" \
            > "$WORK/out" 2> "$WORK/err"; then
        echo "E_FRONT  $rel"
        n_efront=$((n_efront + 1))
        continue
    fi

    # Step 2: cc1plus → .s.
    if ! timeout "$SEA_DG_TIMEOUT" "$CC1PLUS" -quiet -O0 "$base.i" -o "$base.s" \
            > "$WORK/out" 2> "$WORK/err"; then
        echo "E_FRONT  $rel"
        n_efront=$((n_efront + 1))
        continue
    fi
    [ -s "$base.s" ] || { echo "E_FRONT  $rel"; n_efront=$((n_efront + 1)); continue; }

    # Step 3: assemble + link.
    if ! timeout "$SEA_DG_TIMEOUT" cc -no-pie -x assembler "$base.s" -o "$base" \
            -lstdc++ -lm > "$WORK/out" 2> "$WORK/err"; then
        echo "E_CC     $rel"
        n_ecc=$((n_ecc + 1))
        continue
    fi

    # Step 4: run.
    if timeout "$SEA_DG_TIMEOUT" "$base" > /dev/null 2>&1; then
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
echo "=== summary (cc1plus built by sea-front) ==="
echo "  total:   $n_total"
echo "  pass:    $n_pass"
echo "  fail:    $n_fail"
echo "  e_front: $n_efront"
echo "  e_cc:    $n_ecc"
echo "  e_run:   $n_erun"
