// EXPECT: 42
// Virtual call through a const receiver — when sea-front lowers
//   x.m()
// with x being 'const B &' and m being non-const, the emitted call
// passes 'const struct B *' as the implicit 'this' arg to m, whose
// C signature is 'struct B *' (non-const). gcc accepts the implicit
// const-drop with a warning; strict-C back-ends like cproc reject
// the assignment with "discards qualifiers".
//
// Sea-front emit-side fix: strip the const with an explicit cast,
//   m((struct B *)x, ...)
// when the receiver type is const-qualified.
//
// Note: calling a non-const method on a 'const&' is technically
// ill-formed C++; sea-front's sema doesn't reject today (separate
// gap). The emit-side cast ensures the produced C is portable.

struct B {
    virtual int m() { return 42; }
};

int call(const B &x) { return x.m(); }

int main() {
    B b;
    return call(b);
}
