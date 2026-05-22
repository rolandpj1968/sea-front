// EXPECT: 0
// `T::~T() = default;` — out-of-class defaulted destructor
// definition (N4659 §11.4.2 [dcl.fct.def.default]). The parser
// previously stashed the `= default` keyword as an ND_NULLPTR
// initializer on a function-typed var-decl, so no body symbol
// was emitted and the cleanup chain's dtor call failed at link
// time with `undefined reference to T::~T()`.
//
// Pattern: g++.dg/cpp0x/defaulted1.C — `A::~A() = default;`.
// Reduced here to keep the slice's regression test focused on
// the OOL defaulted shape.

int dtors = 0;

struct A {
    int i;
    A() : i(0) {}
    ~A();              // declared in-class, defined OOL as defaulted
};

// Defaulted definition — must produce a linkable D2 symbol.
// Sea-front lowers it to a FUNC_DEF with an empty body; the
// surrounding D1 wrapper chains member dtors around that body.
// Use a side-effect-free + sentinel pattern to verify the
// linkage went through.
A::~A() = default;

struct B {
    A a;
    ~B() { ++dtors; }   // verifies cleanup ran through B too
};

int main() {
    {
        B b;             // ctor: a.i=0 via A's default ctor.
    }                    // scope exit: ~B → ++dtors, then ~A
                         // (defaulted, no-op).
    return dtors == 1 ? 0 : 1;
}
