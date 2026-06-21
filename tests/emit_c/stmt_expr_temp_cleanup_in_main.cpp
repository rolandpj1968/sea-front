// EXPECT: 0
// GCC statement-expression `({ ... })` containing class temporaries
// inside main needs the enclosing function's __SF_unwind /
// __SF_epilogue scaffolding. subtree_has_cleanups must descend into
// ND_STMT_EXPR or main emits without __SF_PROLOGUE while the
// stmt-expr dtor chain still goto's __SF_epilogue. Real-world
// shape: g++.dg/ext/stmtexpr2.C — `({ A(10) + A(11); })` in main.

extern "C" void abort();

int order = 0;
int ctor_count = 0;
int dtor_count = 0;

struct A {
    int v;
    A(int x) : v(x) { ctor_count++; }
    ~A() { dtor_count++; order = order * 10 + v; }
};

int main() {
    ({ A a1(1); A a2(2); a1.v + a2.v; });
    // Two ctors, two dtors (in reverse stmt order: a2 then a1).
    if (ctor_count != 2 || dtor_count != 2) abort();
    if (order != 21) abort();
    return 0;
}
