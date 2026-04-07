// Base-class chain lookup — N4659 §6.4.2 [class.member.lookup].
// A name declared in a base class is visible (without qualification)
// inside a derived-class scope.

struct Base {
    typedef int size_type;
    static int sentinel;
};

struct Derived : public Base {
    void f() {
        // size_type is inherited from Base — should be recognised as
        // a type-name by the parser, so 'size_type n = 0;' is a
        // declaration, not an expression-statement that fails on '__n'.
        size_type n = 0;
        (void)n;
    }
};

// Two-level inheritance.
struct Mid : Derived { typedef long ssize_type; };
struct Leaf : Mid {
    void g() {
        size_type a = 1;     // from Base via Derived
        ssize_type b = 2;    // from Mid
        (void)a; (void)b;
    }
};

// Multiple inheritance — first match wins.
struct A { typedef char value_type; };
struct B { typedef int  other_type; };
struct AB : A, B {
    value_type x;  // from A
    other_type y;  // from B
};
