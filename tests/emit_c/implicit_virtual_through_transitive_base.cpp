// EXPECT: 0
// N4659 §13.3/2 [class.virtual]: a derived-class method with the
// same name + signature as a base virtual IS itself virtual,
// even when the source omits the `virtual` keyword. The match
// can be across MULTIPLE LEVELS of inheritance: an intermediate
// class that doesn't redeclare the method doesn't break the
// virtual chain.
//
// Pre-fix sea-front's implicit-virtual scan only walked the
// derived's DIRECT bases. A class like
//   struct Base { virtual void f(); };
//   struct Mid : Base {};            // doesn't redeclare f
//   struct Leaf : Mid { void f(); }; // implicit override but
//                                    // sea-front missed it
// left Leaf::f as non-virtual, so it wasn't in Leaf's vtable —
// dispatch through `Base *` to a Leaf instance reached Base::f.
//
// Pattern: g++.dg/abi/covariant4.C — EQU::name has no `virtual`
// keyword but Model::name is virtual three levels up.

extern "C" void abort();

struct Base {
    virtual int f() { return 1; }
    virtual ~Base() {}
};

struct Mid : Base {
    // doesn't redeclare f
};

struct Leaf : Mid {
    int f() { return 42; }     // NO `virtual` keyword
};

int call(Base *b) { return b->f(); }

int main() {
    Leaf leaf;
    Base *b = &leaf;
    if (call(b) != 42) return 1;   // virtual dispatch must reach Leaf::f
    return 0;
}
