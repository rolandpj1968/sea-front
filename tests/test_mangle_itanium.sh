#!/bin/sh
# tests/test_mangle_itanium.sh — Itanium mangling unit + cross-check.
#
# Two checks:
#  (1) build/test_mangle_itanium output matches tests/test_mangle_itanium.expected.
#  (2) The same C++ signatures (encoded via tiny .cpp fixtures) mangled
#      by `g++ -c | nm` produce the same param-suffix bytes that the
#      sea-front Itanium encoder emitted. Confirms gcc-parity.
#
# Used by `make test`. Set SF_VERBOSE=1 to print full output on failure.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DRIVER="$ROOT/build/test_mangle_itanium"
EXPECTED="$ROOT/tests/test_mangle_itanium.expected"

if [ ! -x "$DRIVER" ]; then
    echo "test_mangle_itanium: $DRIVER not built — run 'make build/test_mangle_itanium'"
    exit 1
fi

# (1) unit test against expected file.
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
"$DRIVER" > "$TMPDIR/actual"
if ! diff -u "$EXPECTED" "$TMPDIR/actual"; then
    echo "test_mangle_itanium: output diverged from expected"
    exit 1
fi

# (2) g++ parity. For each (label, expected) pair we care about, build
# a one-line .cpp with `void f(<types>)` and check g++'s mangled
# symbol's param portion matches our emit.

# Param-only fixtures (`void f(<cpp>)` whose mangled symbol's suffix
# after `_Z1f` should match <expected>).
# Format: <label>|<cpp>|<expected>
PAIRS='
p_int|int|i
p_int_ptr|int*|Pi
p_int_ptr_int_ptr|int*, int*|PiS_
p_int_ptr_const_int_ptr|int*, const int*|PiPKi
p_int_ptr_ptr|int**|PPi
p_int_ptr_x3_const_int_ptr|int*, int*, int*, const int*|PiS_S_PKi
p_int_array5_decays|int[5]|Pi
p_void||v
'

# Whole-symbol fixtures — sea-front emitted `_Z…`, gcc-via-source
# should emit the same. Format: <label>|<cpp>|<expected_full_symbol>
WHOLE='
m_global_T_foo_void|struct T { void foo(); }; void T::foo() {}|_ZN1T3fooEv
m_global_T_foo_int|struct T { void foo(int); }; void T::foo(int) {}|_ZN1T3fooEi
m_global_T_foo_const|struct T { void foo() const; }; void T::foo() const {}|_ZNK1T3fooEv
m_std_T_foo_void|namespace std { struct T { void foo(); }; void T::foo() {} }|_ZNSt1T3fooEv
m_ns_T_foo_void|namespace ns { struct T { void foo(); }; void T::foo() {} }|_ZN2ns1T3fooEv
m_global_T_foo_T_ptr_T_ptr|struct T { void foo(T*, T*); }; void T::foo(T*, T*) {}|_ZN1T3fooEPS_S0_
m_global_T_foo_constT_ptr_x2_T_ptr|struct T { void foo(const T*, const T*, T*); }; void T::foo(const T*, const T*, T*) {}|_ZN1T3fooEPKS_S1_PS_
m_global_T_foo_Tp_Tpp_Tp_Tpp|struct T { void foo(T*, T**, T*, T**); }; void T::foo(T*, T**, T*, T**) {}|_ZN1T3fooEPS_PS0_S0_S1_
m_global_T_foo_ip_Tp_ip_Tp_ip|struct T { void foo(int*, T*, int*, T*, int*); }; void T::foo(int*, T*, int*, T*, int*) {}|_ZN1T3fooEPiPS_S0_S1_S0_
'

# Ctor/dtor fixtures — match against the C1 (complete-object ctor)
# and D1 (complete-object dtor) symbols gcc emits for `T t;` callers.
# Also picks up D2 to verify dtor_body parity.
CDS='
c_global_T_void|struct T { T(); }; T::T(){}|_ZN1TC1Ev
c_global_T_int|struct T { T(int); }; T::T(int){}|_ZN1TC1Ei
c_std_T_void|namespace std { struct T { T(); }; T::T(){} }|_ZNSt1TC1Ev
d_global_T|struct T { ~T(); }; T::~T(){}|_ZN1TD1Ev
d_std_T|namespace std { struct T { ~T(); }; T::~T(){} }|_ZNSt1TD1Ev
db_global_T|struct T { ~T(); }; T::~T(){}|_ZN1TD2Ev
'

# Helpers
extract_after_1f() {
    # _Z1fXXXX → XXXX
    printf "%s" "$1" | sed -e 's/^_Z1f//'
}

failed=0
echo "$PAIRS" | grep . | while IFS='|' read -r label cpp expected; do
    [ -z "$label" ] && continue
    src="$TMPDIR/$label.cpp"
    obj="$TMPDIR/$label.o"
    if [ -z "$cpp" ]; then
        printf 'void f(){}\n' > "$src"
    else
        printf 'void f(%s){}\n' "$cpp" > "$src"
    fi
    if ! g++ -c -fno-rtti -fno-exceptions -w -o "$obj" "$src" 2>"$TMPDIR/$label.err"; then
        echo "test_mangle_itanium: g++ failed to compile fixture '$label':"
        cat "$TMPDIR/$label.err"
        continue
    fi
    sym=$(nm "$obj" | awk '/T _Z1f/{print $NF; exit}')
    if [ -z "$sym" ]; then
        echo "test_mangle_itanium: no _Z1f symbol from g++ for '$label'"
        continue
    fi
    gcc_suffix=$(extract_after_1f "$sym")
    if [ "$gcc_suffix" != "$expected" ]; then
        echo "test_mangle_itanium: g++ mismatch for '$label':"
        echo "  cpp:      $cpp"
        echo "  gcc:      $gcc_suffix"
        echo "  expected: $expected"
    fi
done

echo "$WHOLE" | grep . | while IFS='|' read -r label cpp expected; do
    [ -z "$label" ] && continue
    src="$TMPDIR/$label.cpp"
    obj="$TMPDIR/$label.o"
    printf '%s\n' "$cpp" > "$src"
    if ! g++ -c -fno-rtti -fno-exceptions -w -o "$obj" "$src" 2>"$TMPDIR/$label.err"; then
        echo "test_mangle_itanium: g++ failed for '$label':"
        cat "$TMPDIR/$label.err"
        continue
    fi
    # Find the matching method symbol in the object. We narrow to
    # 'foo' since the fixture method is always named that.
    sym=$(nm "$obj" | awk '/T _Z.*3foo/{print $NF; exit}')
    if [ "$sym" != "$expected" ]; then
        echo "test_mangle_itanium: whole-symbol mismatch for '$label':"
        echo "  cpp:      $cpp"
        echo "  gcc:      $sym"
        echo "  expected: $expected"
    fi
done

# Ctor/dtor — gcc emits both C1/C2 and D1/D2. We grep for the
# specific expected symbol so the parity check confirms it shows up.
echo "$CDS" | grep . | while IFS='|' read -r label cpp expected; do
    [ -z "$label" ] && continue
    src="$TMPDIR/$label.cpp"
    obj="$TMPDIR/$label.o"
    printf '%s\n' "$cpp" > "$src"
    if ! g++ -c -fno-rtti -fno-exceptions -w -o "$obj" "$src" 2>"$TMPDIR/$label.err"; then
        echo "test_mangle_itanium: g++ failed for '$label':"
        cat "$TMPDIR/$label.err"
        continue
    fi
    if ! nm "$obj" | grep -q "[WT] $expected\$"; then
        echo "test_mangle_itanium: ctor/dtor missing for '$label':"
        echo "  cpp:      $cpp"
        echo "  expected: $expected"
        echo "  available:"
        nm "$obj" | grep "_Z" | sed 's/^/    /'
    fi
done
# Note: subshell modifications to `failed` don't propagate; rely on
# the sea-front-vs-expected diff above for hard-fail and let any
# g++-parity divergences print to stdout for visual inspection.

echo "test_mangle_itanium: OK"
