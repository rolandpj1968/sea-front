// EXPECT: 42
// A virtual method declared 'const' must produce the same mangled
// symbol at both the definition site AND every vtable-slot
// reference. Before the fix, the vtable instance hardcoded
// 'is_const=false' regardless of the declaration, so the slot
// referenced a non-existent non-const symbol while the actual
// method's def was mangled with the _const suffix — link failed.
// Surfaced by gcc 4.8 g++.dg/inherit/covariant17.

struct Base {
    virtual int peek() const { return 1; }
};

struct Derived : Base {
    virtual int peek() const { return 42; }
};

int main() {
    Derived d;
    Base *p = &d;
    return p->peek();
}
