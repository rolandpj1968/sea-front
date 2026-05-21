// EXPECT: 0
// Reference initialised from a comma-expression yielding an lvalue
// — N4659 §8.20/1 [expr.comma]: the comma's result is the rhs
// operand, preserving lvalueness. C makes comma always rvalue, so
// `&(side_effect, m)` is invalid C. The fix is to push the `&`
// inside the comma's rhs: `(side_effect, &m)`.
//
// Three positions where the pattern occurs:
//   - Local variable: `int &r((Foo(), m));`
//   - Class mem-init: `C::C() : r((Foo(), m)) {}`
//   - Class member read of a reference: `if (r != m)` inside a
//     method must deref `this->r` (lowered to T*) before
//     comparing against the int value.
//
// Mirrors g++.dg/other/init2.C.

int side_runs = 0;

void Foo() { ++side_runs; }

int g_fail = 0;

struct C {
    int m;
    int &r;
    C();
};

C::C() : m(1), r((Foo(), m)) {
    m = 10;
    if (r != m)           g_fail = 1;
    else if (&m != &r)    g_fail = 2;
}

int main() {
    int m = 1;
    int &r((Foo(), m));
    m = 10;
    if (r != m)           return 3;
    if (&r != &m)         return 4;
    if (side_runs != 1)   return 5;
    {
        C c;
        if (g_fail)       return g_fail;
    }
    if (side_runs != 2)   return 6;
    return 0;
}
