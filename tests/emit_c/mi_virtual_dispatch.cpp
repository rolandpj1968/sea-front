// EXPECT: 30
// Multi-inheritance virtual dispatch — N4659 §13.3 [class.virtual].
// When a derived class inherits from two polymorphic bases and
// overrides a virtual from each, dispatch via either base pointer
// must reach the derived's override.
//
// In sea-front this requires:
//   1. A secondary vtable per (Derived, non-first-base) pair, typed
//      as the base's vtable struct but populated with thunks for
//      every overridden slot;
//   2. Each thunk adjusts the base pointer down to a Derived pointer
//      via offsetof and forwards to the most-derived implementation;
//   3. The derived ctor installs each secondary vtable into the
//      corresponding base sub-object's vptr after the base ctor
//      ran (which installed the base's own primary vtable).

struct A {
    virtual int f() { return 1; }
    virtual ~A() {}
};

struct B {
    virtual int g() { return 2; }
    virtual ~B() {}
};

struct D : A, B {
    virtual int f() { return 10; }
    virtual int g() { return 20; }
};

int call_f(A *a) { return a->f(); }
int call_g(B *b) { return b->g(); }

int main() {
    D d;
    return call_f(&d) + call_g(&d);   // 10 + 20 = 30
}
