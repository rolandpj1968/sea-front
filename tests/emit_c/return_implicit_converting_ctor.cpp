// EXPECT: 0
// `return e;` where the function returns class C and e has a
// different type E with C-has-ctor(E) — N4659 §11.6.3/5 +
// §16.3.1.5 [over.match.copy]: copy-initialize the return value
// via C's converting ctor. Sea-front emitted `return e;` literally
// which cc rejects ("incompatible types when returning ...").
//
// Materialize via stmt-expr at the return site:
//   ({ C __r = {0}; C_ctor(&__r, &(e)); __r; })
//
// Reduced from g++.dg/init/ref9.C `return basic();` where the
// function returns `ex` and ex has `ex(const basic&)`.

extern "C" void abort();

struct B {
    int n;
    B(int x) : n(x) {}
};

struct A {
    int v;
    A(const B &b) : v(b.n + 100) {}
};

A make_a_via_b() { return B(7); }   /* implicit B → A via A(const B&) */

int main() {
    A a1 = make_a_via_b();
    if (a1.v != 107) abort();
    return 0;
}
