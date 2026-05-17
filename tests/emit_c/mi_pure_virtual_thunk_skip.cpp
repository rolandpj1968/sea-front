// EXPECT: 42
// An intermediate class in a multi-inheritance hierarchy can have
// PURE-VIRTUAL overrides of base methods. Sea-front's MI secondary-
// vtable thunk emit must NOT generate a thunk that calls those
// (no body exists), and the matching slot must fall back to 0
// rather than reference the missing symbol. The original failure
// was a LINK error: 'undefined reference to AB::g() const'.
//
// Surfaced by gcc 4.8 g++.dg/abi/covariant4 ('struct RA :
// public R, public A { virtual RA *clone() const = 0; }').

struct A {
    virtual int f() = 0;
};
struct B {
    virtual int g() = 0;
};

// Intermediate abstract class — overrides both bases' virtuals as
// PURE-VIRTUAL again. Sea-front's MI emit for AB-as-B must skip
// the thunk for g (no AB::g body). Pre-fix, the secondary vtable
// emission produced a thunk symbol that referenced the missing
// AB::g body and the program failed to link.
struct AB : A, B {
    virtual int f() = 0;
    virtual int g() = 0;
};

struct D : AB {
    virtual int f() { return 42; }
    virtual int g() { return 0; }
};

int main() {
    // Dispatch via the primary (offset-0) base chain — exercises
    // D's own primary vtable, which works regardless of any
    // transitive-base secondary-vtable issues. The point of THIS
    // test is the link-success on the intermediate AB's secondary
    // emission, not the dispatch path.
    D d;
    A *a = &d;
    return a->f();
}
