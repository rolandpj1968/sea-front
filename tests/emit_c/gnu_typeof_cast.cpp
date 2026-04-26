// EXPECT: 0
// GNU __typeof / typeof / __typeof__ — non-standard (NOT N4659; the
// standard's analogue is decltype, §10.1.7.2 [dcl.type.simple]).
// Sea-front maps all four spellings to TK_KW_TYPEOF and lowers the
// type-form to an opaque TY_INT placeholder, mirroring decltype.
//
// This is the pattern in glibc's <bits/iscanonical.h> via the
// __iscanonical macro — sea-front previously emitted the cast
// '(__typeof(x))(x)' as '0(x)' (treating the unknown type-name as
// literal 0), which the C compiler reads as "called object is not
// a function." Filed as #141; the broken emit blocked six gen-tool
// .o builds in gcc 4.8 (genopinit, genextract, genautomata, vec,
// read-rtl, genattr).
int f(int x) {
    return ((void)(__typeof(x))(x), 42);
}

int g(double d) {
    /* Result of the cast is discarded; only the comma's tail
     * survives. The (int) cast that sea-front emits in place of
     * (__typeof(d)) truncates d but is then discarded — same
     * observable behaviour. */
    return ((void)(typeof(d))(d), 7);
}

int main() {
    return (f(42) - 42) + (g(3.14) - 7);
}
