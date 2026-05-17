// EXPECT: 42
// An abstract base with pure-virtual methods must still expose its
// vtable struct + vptr field so a derived class's secondary vtable
// for that base can be sized correctly. Previously sea-front
// over-conservatively gated both on 'any_virtual_has_body' — false
// for pure-virtual bases — so derived classes' MI secondary
// vtable-instance allocations referenced an incomplete type.
//
// Surfaced by gcc 4.8 g++.dg/opt/thunk1 (and the broader 'vtable_for
// has incomplete type' cluster).

struct A {
    virtual int f() = 0;          // pure
};

struct B {
    virtual int g() = 0;          // pure
};

struct D : A, B {
    virtual int f() { return 10; }
    virtual int g() { return 32; }
};

int main() {
    D d;
    A *a = &d;
    B *b = &d;
    return a->f() + b->g();       // 10 + 32 = 42
}
