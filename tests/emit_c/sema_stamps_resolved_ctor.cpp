// EXPECT: 0
// Regression test for the first stage of resolve-everything-before-emit:
// sema runs ctor overload resolution for class type-calls and stamps the
// winner on n->call.resolved_ctor; codegen reads it instead of running
// overload resolution at emit time.
//
// Functionally a no-op vs. the previous emit-only resolution path — this
// test exists to anchor the architectural change with a coverage point.
// Exercises class type-calls with multiple ctor candidates (overload
// resolution must pick the right one). N4659 §16.3 [over.match].

extern "C" void abort();

struct Pair {
    int a;
    int b;
    Pair() : a(-1), b(-1) { }
    Pair(int x) : a(x), b(0) { }
    Pair(int x, int y) : a(x), b(y) { }
};

int main() {
    /* Each of these is a class type-call:
     *   Pair(5)      → ctor(int)
     *   Pair(7, 11)  → ctor(int, int)
     *   Pair()       → default ctor
     * Sema picks each winner; codegen emits the corresponding call
     * via the stamped resolved_ctor. */
    Pair p1 = Pair(5);
    Pair p2 = Pair(7, 11);
    Pair p3 = Pair();
    if (p1.a != 5  || p1.b != 0)  abort();
    if (p2.a != 7  || p2.b != 11) abort();
    if (p3.a != -1 || p3.b != -1) abort();
    return 0;
}
