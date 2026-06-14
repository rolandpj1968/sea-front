// EXPECT: 0
// `delete expr` must evaluate `expr` exactly once even when the
// pointee has a virtual destructor / non-trivial destructor. The
// old emit fanned `expr` out across the null-guard, the vptr
// load, the __dtor call, and the operator-delete call — so an
// `expr` with side effects (like a function call) would fire up
// to four times. Bind to a stmt-expr-local so the operand
// evaluates once.
//
// Reduced from g++.dg/expr/delete2.C. N4659 §8.3.5 [expr.delete]:
// "the operand of the delete-expression ... is evaluated."
// (singular).

extern "C" void abort(void);

struct A {
    virtual ~A() {}
};

A *g_p;
int g_count;

A *f() {
    ++g_count;
    return g_p;
}

int main() {
    g_p = new A;
    delete f();
    if (g_count != 1) abort();
    return 0;
}
