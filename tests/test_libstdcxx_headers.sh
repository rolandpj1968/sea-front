#!/bin/sh
# Smoke test: parse libstdc++ headers end-to-end via mcpp + sea-front.
#
# For each header, write '#include <X>', preprocess with mcpp in C++
# mode (-+), and feed the result to sea-front. Pass/fail tracked per
# header. Not gated; reports counts.
#
# Headers are listed below — the 12 known-passing ones from the
# pre-existing parser work, plus a "stretch" set we want to start
# making pass.

set -e

SEA_FRONT="${1:-build/sea-front}"
MCPP="${2:-build/mcpp-bin}"

INCS="-I/usr/include/c++/13 \
      -I/usr/include/x86_64-linux-gnu/c++/13 \
      -I/usr/include/c++/13/backward \
      -I/usr/lib/gcc/x86_64-linux-gnu/13/include \
      -I/usr/include/x86_64-linux-gnu \
      -I/usr/include"

# GCC-builtin type macros that mcpp doesn't predefine but libstdc++
# headers reference (e.g. '(__UINTPTR_TYPE__)x' casts).
#
# We only need PARSER correctness here, not size accuracy — sea-front
# is about transpilation shape, not numeric semantics. So each macro
# expands to a single built-in type token that the parser accepts in
# type position. The actual sizes are wrong but the parser doesn't
# care.
GCC_TYPE_DEFS="-D__SIZE_TYPE__=long \
               -D__PTRDIFF_TYPE__=long \
               -D__UINTPTR_TYPE__=long \
               -D__INTPTR_TYPE__=long \
               -D__INTMAX_TYPE__=long \
               -D__UINTMAX_TYPE__=long \
               -D__INT8_TYPE__=char \
               -D__UINT8_TYPE__=char \
               -D__INT16_TYPE__=short \
               -D__UINT16_TYPE__=short \
               -D__INT32_TYPE__=int \
               -D__UINT32_TYPE__=int \
               -D__INT64_TYPE__=long \
               -D__UINT64_TYPE__=long \
               -D__WCHAR_TYPE__=int \
               -D__WINT_TYPE__=int \
               -D__CHAR16_TYPE__=short \
               -D__CHAR32_TYPE__=int"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN_DIR="$REPO_ROOT/gen/headers"
rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"

# Gated headers — must always parse cleanly with -V201103L (C++11).
# Original target was 12 from the early parser-grind era; map and
# set rejoin the gated set now that explicit-dtor-via-template-id
# parses ('p->~Class<args>()').
#
# memory is the only original-12 still in stretch — it has a known
# corner case ('this->Class<T>::operator=(...)') that's a focused
# parser fix for a future session.
PASS_HEADERS="vector string map set algorithm iostream unordered_map tuple thread chrono sstream fstream"

# Stretch headers — currently failing or unverified. We track these
# separately so we can see when one moves from FAIL to OK without
# the gated suite turning red.
STRETCH_HEADERS="memory mutex functional deque list array bitset queue stack iomanip iterator type_traits utility numeric optional variant"

PASS=0
FAIL=0

run_one() {
    header="$1"
    label="$2"
    src="$GEN_DIR/${header}.cpp"
    pre="$GEN_DIR/${header}.i"
    err="$GEN_DIR/${header}.err"
    echo "#include <$header>" > "$src"
    if ! "$MCPP" -+ -W0 -V201103L $INCS $GCC_TYPE_DEFS "$src" > "$pre" 2>"$err.mcpp"; then
        printf "  %-20s SKIP   (mcpp failed)\n" "$header"
        return 1
    fi
    if "$SEA_FRONT" "$pre" > /dev/null 2>"$err"; then
        printf "  %-20s OK     (%d lines)\n" "$header" "$(wc -l <"$pre")"
        return 0
    else
        printf "  %-20s FAIL   %s\n" "$header" "$(head -1 "$err")"
        return 1
    fi
}

echo "=== libstdc++ headers (gated — must pass) ==="
for h in $PASS_HEADERS; do
    if run_one "$h" "gated"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "=== libstdc++ headers (stretch — informational) ==="
SPASS=0
SFAIL=0
for h in $STRETCH_HEADERS; do
    if run_one "$h" "stretch"; then
        SPASS=$((SPASS + 1))
    else
        SFAIL=$((SFAIL + 1))
    fi
done

echo ""
echo "libstdc++ gated:    $PASS passed, $FAIL failed"
echo "libstdc++ stretch:  $SPASS passed, $SFAIL failed"

# Only the gated set fails the test.
[ "$FAIL" -eq 0 ]
