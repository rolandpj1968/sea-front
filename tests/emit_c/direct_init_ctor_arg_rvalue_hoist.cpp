// EXPECT: 0
// `A a(B(13))` — direct-initialize a with a class-rvalue arg to a
// converting ctor `A(const B&)`. Sea-front's var-decl emit passes
// ctor_args through emit_arg_for_param which wraps ref-typed args
// in `&(arg)`. For a call rvalue `B(13)`, `&(B(13))` is invalid C
// ("lvalue required as unary '&' operand").
//
// hoist_stmt_temps for ND_VAR_DECL now walks ctor_args alongside
// the existing init walk, so each class-temp arg gets pre-hoisted
// into a named local and the `&(local)` lowering becomes valid.

extern "C" void abort();

struct B {
    int n;
    B(int x) : n(x) {}
};

struct A {
    int v;
    A(const B &b) : v(b.n + 100) {}
};

int main() {
    A a(B(13));        /* class-rvalue arg to a const-ref ctor param */
    if (a.v != 113) abort();
    return 0;
}
