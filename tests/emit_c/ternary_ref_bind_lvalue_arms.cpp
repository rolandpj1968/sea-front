// EXPECT: 0
// `const T& r = cond ? *p : g;` — binding a reference to a ternary
// whose arms are both lvalues. Sea-front lowers refs to pointers
// and was emitting `&(cond ? *p : g)` — invalid C, since C `?:` is
// always an rvalue ('lvalue required as unary "&" operand').
//
// Push the `&` inside each arm: `(cond ? &*p : &g)` → `(cond ? p : &g)`.
// For class-rvalue arms (e.g. `f()` returning T by value), sea-front
// hoists into a pending-assign temp; route the `&` into the comma
// via g_ref_bind_amp_into_hoist so the comma emits as
// `(temp = call(), &temp)` instead of `&(temp = call(), temp)`.
//
// Reduced from g++.dg/init/ref16.C (lvalue-arms only; the rvalue-arm
// shape exposes a separate return-by-value cleanup bug).

extern "C" void abort();

struct A { int v; };

A globalA = { 42 };
A *pA = &globalA;
A otherA = { 7 };

int main() {
    /* lvalue arms — both `*pA` and `otherA` are lvalues; the ternary
     * yields an lvalue too. The fix lets `&` push inside the arms. */
    const A &r1 = (1 ? *pA : otherA);
    if (r1.v != 42) abort();

    const A &r2 = (0 ? *pA : otherA);
    if (r2.v != 7) abort();

    /* Verify the binding is to the actual referenced object, not
     * a copy. */
    pA->v = 99;
    if (r1.v != 99) abort();

    return 0;
}
