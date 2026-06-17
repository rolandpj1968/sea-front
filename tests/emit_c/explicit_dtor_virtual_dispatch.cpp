// EXPECT: 0
// `p->~A()` where A has a virtual dtor must dispatch through the
// vptr to run the dynamic-type's destructor — N4659 §15.4/10
// [class.dtor]. Sea-front used to emit a direct call to A's wrapper
// `_ZN1AD1Ev(a)`, which always runs A's body regardless of dynamic
// type — overriding dtors in derived classes never fired.
//
// Reduced from g++.dg/overload/virtual2.C — also adds:
//   `::operator delete(p)` for non-void p now casts to void* (matches
//   the implicit conversion C++ inserts).

extern "C" void abort();

bool b_dtor_ran = false;

struct A {
    virtual ~A() {}
};

struct B : A {
    virtual ~B() { b_dtor_ran = true; }
};

int main() {
    B *bp = new B;
    A *ap = bp;
    ap->~A();                          /* virtual dispatch → ~B */
    if (!b_dtor_ran) abort();
    ::operator delete(bp);             /* non-void ptr → cast to void* */
    return 0;
}
