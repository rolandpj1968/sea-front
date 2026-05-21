// EXPECT: 0
// Free function defined inside a namespace: the def and the
// qualified call site must agree on the mangled symbol. The call
// site has always emitted Itanium namespace-mangled
// `_ZN<ns_len><ns_name><fn_len><fn_name>...`. Before 8935f12 the
// def emitted bare `<fn_name>`, so the link failed with
// "undefined reference to ns::fn".
//
// Pattern: g++.dg/ext/fnname1.C is the dg integration coverage;
// this isolates the single-level case as a fast unit-test.
//
// Multi-level namespace `outer::inner::f` currently captures only
// the innermost namespace name (ns_token is a single Token, not a
// chain) — call site and def site are both broken consistently so
// the symbols match and the test would link. Not exercised here
// because the wrong-but-consistent mangling makes it a poor
// regression signal.

namespace alpha {
    int direct() { return 42; }
}

namespace beta {
    int via_arg(int x) { return x * 2; }
}

int main() {
    if (alpha::direct() != 42) return 1;
    if (beta::via_arg(7) != 14) return 2;
    return 0;
}
