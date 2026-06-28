// EXPECT: 0
// Implicit copy ctor synthesised for a derived class with a base that
// has a template ctor. Per N4659 §15.8.1/9 [class.copy.ctor]/9 the
// template doesn't count AS the copy ctor, but per §16.3 [over.match]
// it IS in the candidate set for direct-initialising the base sub-
// object. Identity ref binding (T& deduced to A& for non-const arg)
// beats qual ref binding (const A&) per §16.3.3.2.3 [over.ics.rank];
// the template ctor wins.
//
// Pattern: g++.dg/cpp0x/implicit2.C.

extern "C" void abort();

int r = 1;

struct A {
    A() {}
    A(const A&) {}
    template <class T> A(T& t) { r = 0; }
};

struct B {
    B() {}
    B(B&) {}
};

struct C : A, B { };

int main() {
    C c;
    (C(c));
    return r;
}
