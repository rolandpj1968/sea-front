// EXPECT: 0
// Template-id use inside a GCC statement-expression `({ ... })` must
// trigger template instantiation. Without descending into ND_STMT_EXPR
// during template-instantiation discovery (instantiate.c
// collect_from_node) the template-id is invisible — codegen later
// emits the bare ident as a call to an undeclared free function and
// the link fails with "undefined reference to A".
//
// Real-world hit: g++.dg/ext/stmtexpr1.C — `({ A<1>(); A<2>(); ;})`.

extern "C" void abort();

int ctor_calls = 0;
int sum = 0;

template <int I> struct A {
    A() { ctor_calls++; sum += I; }
};

int main() {
    ({ A<1>(); A<2>(); ; });
    ({ A<3>(); ; });
    if (ctor_calls != 3) abort();
    if (sum != 1 + 2 + 3) abort();
    return 0;
}
