// EXPECT: 0
// Aggregate-init `T x = {e1, e2, ...};` where T has class members
// with user-defined copy ctors: each member must be copy-
// constructed via its user ctor (not bitwise C struct init), and
// if one of those ctors throws, the already-constructed members
// must be destroyed in reverse before the exception propagates.
// N4659 §15.6.2/12 [class.base.init] + §15.2/2 [except.ctor].
//
// Pattern: g++.dg/eh/partial1.C. Without per-member ctor + partial-
// destruction, sea-front previously emitted `struct C c = {b1, a,
// b2};` (bitwise) and the user copy ctors never ran — the test
// passed by accident before the flat-block cleanup fix exposed
// the imbalance.

int bs = 0;

struct A {
    A() {}
    A(const A&) { throw 1; }     // throws on copy
};

struct B {
    B()              { ++bs; }
    B(const B&)      { ++bs; }    // counts copies
    ~B()             { --bs; }
};

struct C {
    B b1;
    A a;
    B b2;
};

int main() {
    {
        B b1, b2;                 // bs = 2
        A a;
        try {
            C c = { b1, a, b2 };  // c.b1 copy: bs=3; c.a copy: throws.
                                  // partial destruction: ~c.b1 → bs=2.
        } catch (...) {}
    }
    // outer scope: ~b2 (bs=1), ~b1 (bs=0).
    return bs == 0 ? 0 : 1;
}
