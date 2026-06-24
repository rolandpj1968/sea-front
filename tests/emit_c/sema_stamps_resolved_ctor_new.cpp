// EXPECT: 0
// Third stage of resolve-everything-before-emit: sema stamps the ctor
// overload winner for `new T(args)` onto cast.resolved_ctor; codegen
// reads the stamp instead of re-running overload resolution at emit
// time. Same pattern as the var-decl direct-init stamp, applied to the
// new-expression shape. N4659 §16.3 [over.match] / §8.3.4 [expr.new].

extern "C" void abort();

struct Pair {
    int a;
    int b;
    Pair() : a(-1), b(-1) { }
    Pair(int x) : a(x), b(0) { }
    Pair(int x, int y) : a(x), b(y) { }
};

int main() {
    /* Each new-expr must pick the matching ctor via the stamp:
     *   new Pair(5)      → ctor(int)
     *   new Pair(7, 11)  → ctor(int, int)
     *   new Pair()       → default ctor (no stamp; default-ctor path) */
    Pair *p1 = new Pair(5);
    Pair *p2 = new Pair(7, 11);
    Pair *p3 = new Pair();
    int rc = (p1->a == 5 && p1->b == 0 &&
              p2->a == 7 && p2->b == 11 &&
              p3->a == -1 && p3->b == -1) ? 0 : 1;
    delete p1;
    delete p2;
    delete p3;
    if (rc) abort();
    return rc;
}
