// EXPECT: 0
// `B b = { T() };` — aggregate init where an element is a
// functional-cast `T()` for a class T whose member needs T's
// ctor to run on the FINAL storage (e.g. T records `this` for
// later identity checks). The compiler is required by N4659
// §15.8.3 [class.copy.elision] to elide the implicit copy and
// construct T DIRECTLY into the aggregate slot.
//
// Sea-front used to emit `b = {(T){0}}` (compound literal, no
// ctor body run) which left T's `p = this` unset, then the
// dtor's `if (this != p) abort()` blew up.
//
// Pattern: g++.dg/init/aggr2.C.

extern "C" void abort();
int dctors = 0;

struct A {
    static A *p;
    A() { p = this; }
    A(const A &);             // declared; with elision it must never link
    ~A() {
        if (this != p) abort();
        ++dctors;
    }
};
A *A::p;

struct B { A a; };

int main() {
    {
        B b = { A() };          // A() must construct directly into b.a
        if (A::p != &b.a) return 1;
    }
    // b's scope exit ran ~A on &b.a (which equals A::p at that moment).
    if (dctors != 1) return 2;
    return 0;
}
