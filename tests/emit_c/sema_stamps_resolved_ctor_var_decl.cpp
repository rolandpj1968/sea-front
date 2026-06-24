// EXPECT: 0
// Second stage of resolve-everything-before-emit: sema stamps the ctor
// overload winner for var-decl direct-init `T x(args)` and `T x{args}`
// onto var_decl.resolved_ctor; codegen reads the stamp instead of
// re-running overload resolution at emit time.
//
// Functionally equivalent to the previous emit-only path — this exists
// to anchor the architectural change with a coverage point.
// Exercises class var-decl direct-init with multiple ctor candidates
// (overload resolution must pick the right one). N4659 §16.3 [over.match].

extern "C" void abort();

struct Pair {
    int a;
    int b;
    Pair() : a(-1), b(-1) { }
    Pair(int x) : a(x), b(0) { }
    Pair(int x, int y) : a(x), b(y) { }
};

int main() {
    /* Each of these is a class var-decl direct-init:
     *   Pair p1(5)      → ctor(int)
     *   Pair p2(7, 11)  → ctor(int, int)
     *   Pair p3{42}     → ctor(int) via brace-init
     * Sema picks each winner; codegen emits the corresponding call
     * via the stamped var_decl.resolved_ctor. */
    Pair p1(5);
    Pair p2(7, 11);
    Pair p3{42};
    if (p1.a != 5  || p1.b != 0)  abort();
    if (p2.a != 7  || p2.b != 11) abort();
    if (p3.a != 42 || p3.b != 0)  abort();
    return 0;
}
