// EXPECT: 0
// MI covariant-return thunks for both POINTER and REFERENCE
// returns. Pre-fix sea-front:
//   1. Skipped return-adjust for reference-typed slots (only
//      pointer-to-class triggered the adjustment), so dispatch
//      through `B*` to a `C&` override returned a C* without
//      shifting to the B subobject.
//   2. Adjusted null pointers unconditionally: `return 0` from
//      a covariant override became `(B*)offset` (non-null bogus)
//      after the thunk added the subobject offset.
//   3. Emitted `&<call-returning-ref>` literally — invalid C
//      because the call's result is an rvalue.
//
// Pattern: g++.dg/abi/covariant5.C — "Covariant return pointer
// could be null".

struct A { virtual void One(); };
struct B {
    virtual B *Two();
    virtual B &Three();
};
struct C : A, B {
    virtual C *Two();
    virtual C &Three();
};

void A::One() {}
B *B::Two()   { return this; }
B &B::Three() { return *this; }
C *C::Two()   { return 0; }                  // returns null
C &C::Three() { return *(C *)0; }            // bogus null reference

B *Foo(B *b) { return b->Two(); }
B &Bar(B *b) { return b->Three(); }

int main() {
    C c;

    // C::Two returns null; thunk must NOT adjust → Foo returns null.
    if (Foo(&c)) return 1;

    // C::Three returns *(C*)0; thunk DOES adjust unconditionally
    // (ref semantics). &Bar(&c) — & of a ref-returning call,
    // which sea-front lowers to just the call expression (the
    // ref is already a pointer at C level). Must yield non-null
    // (the adjusted pointer).
    if (!&Bar(&c)) return 2;

    return 0;
}
