// EXPECT: 0
// `auto x(expr);` (direct-initialization form) must deduce
// auto from expr's type — same as `auto x = expr;`. N4659
// §10.1.7.4.1 [dcl.spec.auto.deduce] applies to both
// initialization forms.
//
// Sema's auto-deduction only fired when var_decl.init was set;
// the direct-init form puts the arg in var_decl.ctor_args[0]
// and the auto-deduction trigger missed it. Result: auto stayed
// as the int placeholder type and downstream init-type-checks
// rejected the assignment.
//
// Reduced from g++.dg/cpp0x/auto14.C (PR c++/40306, c++/40307).

extern "C" void abort();

template<typename T> struct A {
    int v;
    A() : v(0) {}
};

int main() {
    A<int> a;
    auto b = a;     /* copy-init — already worked */
    auto c(a);      /* direct-init — the regression case */
    if (b.v != 0) abort();
    if (c.v != 0) abort();
    return 0;
}
