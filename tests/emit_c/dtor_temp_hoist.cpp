// EXPECT: 6
// Slice D-Hoist: a class-typed expression at a "temporary position"
// (here, the result of make() in the rvalue of an assignment) is
// hoisted to a synthesized local. The local is pushed onto the
// cleanup chain, so its dtor fires when the enclosing block exits.
//
// Without D-Hoist the temp would be leaked: make()'s return value
// would be consumed by .v, the C struct would vanish without
// destruction, and g would stay 0 — the test would return 5.
//
// With D-Hoist the temp's dtor fires at the inner block's exit
// (g = 1), then the outer return reads g and computes x + g = 6.
//
// Lifetime divergence from C++: temps are scoped to their
// enclosing block, not to the full-expression. The user's nested
// block here exists explicitly so the dtor's side effect is
// observable BEFORE the next outer statement reads g. Without
// the wrapper block, our extended-lifetime model would defer the
// dtor to end-of-main, and 'return x + g' would read g while
// it was still 0. Mini-block isolation per full-expression is
// a possible future refinement.
int g = 0;

struct T {
    int v;
    ~T() { g = g + 1; }
};

T make() {
    T t;
    t.v = 5;
    return t;
}

int main() {
    int x;
    {
        x = make().v;
    }
    return x + g;
}
