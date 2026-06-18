// EXPECT: 0
// Derived class's primary vtable must mirror the vptr-owner's slot
// layout, otherwise dispatch through a base-typed vptr reads slots
// at the wrong offset. Sea-front used to emit only the derived
// class's OWN virtual members into its vtable struct, so e.g.:
//     class A { virtual float bar(float); virtual int foo(int); };
//     class B : A { virtual int foo(int); };
//   produced `struct sf__B__vtable { int (*foo)(struct sf__B*, int); }`
// with foo at offset 0, while the vptr is typed `sf__A__vtable*`
// (foo at offset 1) — so dispatch through an A* view read past
// sf__B__vtable_instance's storage and crashed.
//
// N4659 §13.3 [class.virtual] — the derived vtable inherits the
// base's layout. Reduced from g++.dg/ipa/devirt-3.C.

extern "C" void abort();

struct A {
    int data;
    virtual float distraction(float f) { return f / 2; }
    virtual int   foo(int i)           { return i + 1; }
};

struct B : A {
    virtual int   foo(int i) { return i + 2; }
};

static int middleman(A &obj, int i) {
    return obj.foo(i);
}

int main() {
    B b;
    if (middleman(b, 1) != 3) abort();  /* B::foo via A& */
    A a;
    if (middleman(a, 1) != 2) abort();  /* A::foo via A& */
    if (b.distraction(8.0f) != 4.0f) abort();  /* inherited, not overridden */
    return 0;
}
