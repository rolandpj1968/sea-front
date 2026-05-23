// EXPECT: 0
// `using Base::foo;` inside Derived injects Base's method into
// Derived's scope. A call `d.foo()` must mangle as `Base::foo`
// (not `Derived::foo`) and pass `&d.__sf_base` as the `this`
// argument so the base subobject's pointer reaches the body.
// N4659 §10.3.3 [namespace.udecl]. Pre-fix sea-front emitted
// `Derived::foo(&d)` — linker undefined-reference.
//
// Pattern: g++.dg/inherit/using2.C.

struct Base {
    int magic;
    Base() : magic(42) {}
    int getMagic() { return magic; }
};

struct Derived : Base {
    using Base::getMagic;
    int unrelated;
    Derived() : unrelated(7) {}
};

int main() {
    Derived d;
    if (d.getMagic() != 42) return 1;
    if (d.unrelated != 7)   return 2;
    return 0;
}
