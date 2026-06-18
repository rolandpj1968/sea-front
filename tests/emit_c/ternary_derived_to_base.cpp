// EXPECT: 0
// `cond ? D-rvalue : B-lvalue` (D : public B) has common type B per
// N4659 §8.16/6 [expr.cond] — the derived arm is converted to the
// base. At C level both arms of `?:` must be the same type or cc
// rejects with "type mismatch in conditional expression". Sea-front
// used to emit the literal `(cond ? d_expr : b_expr)`, which cc
// flagged whenever D-pointer-and-B-pointer or D-value-and-B-value
// pairs appeared.
//
// Lower the derived arm via `(expr).__sf_base[.__sf_base...]` so
// both arms have base type — C99 §6.2.4 keeps the call-result temp
// alive until end of full-expression, so the base-access on a
// returned rvalue is valid.
//
// Reduced from g++.dg/expr/cond6.C.

extern "C" void abort();

int b_assign_count = 0;
int b_copy_count = 0;

/* No user operator= — keep b = ternary as plain C struct copy.
 * The fix this test verifies is the ternary EMIT walking the
 * derived arm to its base subobject (so both arms are the same
 * C type and cc doesn't reject the ternary as type-mismatched).
 * Lifting to copy semantics keeps the test exercising only the
 * ternary fix, not the parallel op= dispatch path (which has its
 * own ref-arg-lowering for class-typed ternary args). */
struct B {
    int x;
    B() : x(0) {}
};

struct D : public B {
    int y;
    D() : y(0) { x = 7; }
};

D make_d() {
    D d;
    d.x = 42;
    return d;
}

int main() {
    B b;
    b = (1 ? make_d() : b);   /* common type B; D arm → base subobject */
    if (b.x != 42) abort();

    /* Mirror: B in then arm, D in else — same chain, opposite slot */
    b.x = 0;
    b = (0 ? b : make_d());
    if (b.x != 42) abort();

    (void)b_assign_count; (void)b_copy_count;
    return 0;
}
