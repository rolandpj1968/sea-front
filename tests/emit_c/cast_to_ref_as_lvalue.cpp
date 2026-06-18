// EXPECT: 0
// `static_cast<Base&>(sub) = rhs;` — a ref-typed cast is an lvalue
// per N4659 §8.16/2 [expr.cond]. Sea-front lowers refs to pointers
// so the cast emits as `(struct sf__Base *)&(sub).__sf_base` — a
// pointer rvalue. As an assignment target cc rejected with "lvalue
// required as left operand of assignment".
//
// Lower as `*(<cast>) = rhs;` — deref the pointer to get back an
// lvalue. Works for both scalar and struct RHS (C struct assignment
// is legal); user-defined operator= dispatch on the deref'd cast
// is a deeper slice not covered here.
//
// Reduced from g++.dg/expr/assign1.C.

extern "C" void abort();

struct Base { int i; char c; };
struct Sub : Base { char d; };

int main() {
    Sub sub;
    sub.i = 0; sub.c = 0; sub.d = 99;
    Base b;
    b.i = 42; b.c = 'x';

    static_cast<Base &>(sub) = b;   /* assign to base subobject only */

    if (sub.i != 42) abort();
    if (sub.c != 'x') abort();
    if (sub.d != 99) abort();       /* sub-specific field untouched */
    return 0;
}
