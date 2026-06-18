// EXPECT: 0
// GCC statement-expression `({ stmts; expr; })` containing class
// temporaries built via functional cast: the parser produces
// ND_STMT_EXPR wrapping an ND_BLOCK. Sema's visit switch must
// descend into the block so the inner `T(args)` ND_CALLs reach
// visit_call and pick up is_type_call. Without it they emit as
// bare unmangled function calls — cc rejects with `invalid
// initializer` or link-fails on the bare type-name.
//
// Pattern reduced from g++.dg/ext/stmtexpr2.C — that test exercises
// `A(10) + A(11)` inside a stmt-expr, which combines functional-cast
// classification with operator+ overload dispatch on class temps.
// This unit test isolates the sema-visit descent: a class with both
// a default ctor and a value ctor, so the rewrite_copy_init path
// (which gates on has_default_ctor) takes over and copy-elides into
// the value ctor at the var-decl init slot.

extern "C" void abort();

int constructs = 0;
int last_val = -1;

struct A {
    int v;
    A() : v(0) { ++constructs; }
    A(int x) : v(x) { ++constructs; last_val = x; }
};

int main() {
    /* Inner `A(7)` is a functional cast inside the stmt-expr's body.
       Without sema descending into ND_STMT_EXPR.block, the call's
       is_type_call stays false and emit drops a bare `A(7)`. */
    int r = ({ A x = A(7); x.v; });
    if (r != 7) abort();
    if (last_val != 7) abort();

    /* Two stmt-exprs, each constructing a temp — verifies that each
       inner expr is independently classified. */
    int s = ({ A y = A(11); y.v; }) + ({ A z = A(31); z.v; });
    if (s != 42) abort();
    if (last_val != 31) abort();

    return 0;
}
