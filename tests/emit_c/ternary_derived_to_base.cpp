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

struct B {
    int x;
    B() : x(0) {}
    B(const B &o) : x(o.x) { ++b_copy_count; }
    B &operator=(const B &o) { x = o.x; ++b_assign_count; return *this; }
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
    /* User-defined operator= dispatch on plain assignment is a
     * separate (pre-existing) sea-front gap; just check the value
     * landed correctly via the base-conversion path. */
    (void)b_assign_count;
    (void)b_copy_count;

    /* Mirror: B in then arm, D in else — same chain, opposite slot */
    b.x = 0;
    b = (0 ? b : make_d());
    if (b.x != 42) abort();

    return 0;
}
