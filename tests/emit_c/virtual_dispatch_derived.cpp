// EXPECT: 42
// Virtual dispatch through a derived class — the derived struct does
// not carry its own __sf_vptr; the field lives at the polymorphic
// root (here A), and access from B goes through the offset-0 base
// subobject. N4659 §13.3 [class.virtual] / §13.5 [class.derived].

struct A {
    virtual int f() { return 0; }
    virtual ~A() {}
};

struct B : A {
    int n;
    B(int x) : n(x) {}
    virtual int f() { return n; }
};

int main() {
    B b(42);
    A *p = &b;
    return p->f();   // dispatches through the vptr in the A subobject of b
}
