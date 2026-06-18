// EXPECT: 0
// Plain `=` on a class with a user-defined `operator=` must
// dispatch to that operator, not emit a C bitwise struct copy.
// N4659 §15.8 [class.copy] — if the class declares its own
// operator=, that's the function that runs; the implicit
// memberwise assign only fires when no user operator= exists.
//
// Sea-front's ND_ASSIGN emit already had a compound-assignment
// dispatch (a += b → Class__plus_assign(&a, b)), and the array-
// memberwise-assign fallback already gated on
// !class_has_user_op_assign expecting an upstream dispatch.
// Plain `=` was missing — added a parallel branch that resolves
// the user operator= and emits Class__op_assign(&lhs, &rhs).
//
// This unblocks any test that depends on the user-side effects of
// operator= — counters, linked-list bookkeeping, refcounts.
// Pattern: g++.dg/torture/pr40389.C, g++.dg/init/assign1.C
// (virtual-inheritance shape), etc.

extern "C" void abort();

int assigns = 0;
int copies  = 0;

struct A {
    int v;
    A() : v(0) {}
    A(const A &o) : v(o.v) { ++copies; }
    A &operator=(const A &o) { v = o.v + 100; ++assigns; return *this; }
};

int main() {
    A a, b;
    b.v = 7;

    /* Statement-discarded plain `=` — most common shape. */
    a = b;
    if (a.v != 107) abort();
    if (assigns != 1) abort();

    /* Plain `=` inside a conditional — return value of op= is the
     * LHS pointer (`T*` lowering of `T&`), always non-NULL → truthy. */
    A c;
    c.v = 5;
    a = c;
    if (a.v != 105) abort();
    if (assigns != 2) abort();

    /* Make sure NOT-class assignment (int = int) still uses native
     * C `=` and doesn't accidentally dispatch through anything. */
    int x = 0; x = 42;
    if (x != 42) abort();

    return 0;
}
