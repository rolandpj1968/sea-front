// EXPECT: 142
// Non-virtual single inheritance layout — N4659 §11 [class.derived],
// §15.6.2 [class.base.init], §15.4 [class.dtor].
//
// First slice: derived classes embed their direct base subobjects
// in declaration order, ctors chain to base ctors first, dtors
// chain to base dtors last, inherited members are accessed via
// '__sf_base.' walks. No virtual cross-class override yet (the
// next slice).
//
// Verified end-to-end:
//   - Base() runs first when Derived() is constructed.
//   - Inherited 'get()' is callable as 'get()' from Derived's body
//     (lookup walks the base chain) and lowers to a call on
//     '&this->__sf_base'.
//   - Inherited field 'x' is accessed via 'this->__sf_base.x'.
//   - 42 + 100 = 142, exit code matches.

struct Base {
    int x;
    Base() : x(42) {}
    int get() { return x; }
};

struct Derived : Base {
    int y;
    Derived() : y(100) {}
    int total() { return get() + y; }
};

int main() {
    Derived d;
    return d.total();
}
