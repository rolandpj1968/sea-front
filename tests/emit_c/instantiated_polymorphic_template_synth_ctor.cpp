// EXPECT: 0
// An instantiated class template that inherits virtual functions
// from a non-template base — `TPL<T> : B` where B has virtuals —
// needs a SYNTHESISED default ctor so the vptr gets installed.
// Sea-front's vptr-install lives in the ctor wrapper; without
// the synthesised ctor, `TPL<int> i;` leaves __sf_vptr
// uninitialised and the first virtual call segfaults.
//
// Pre-fix the instantiation pass at template/instantiate.c
// derived has_default_ctor only from explicitly-declared user
// ctors on the cloned class body. type.c's "polymorphic-no-user-
// ctor → synthesise default ctor" rule (N4659 §15.1/4) needs to
// fire for instantiated copies too.
//
// Pattern: g++.dg/template/qual2.C.

extern "C" void abort();

struct B {
    virtual int answer() { return 42; }
};

template <class T>
struct D : B {
    int answer() { return 7; }      // implicit-virtual override
};

int main() {
    D<int> d;
    B *b = &d;
    // Virtual dispatch through B* must reach D::answer (== 7),
    // not B::answer (42).
    if (b->answer() != 7) abort();
    return 0;
}
