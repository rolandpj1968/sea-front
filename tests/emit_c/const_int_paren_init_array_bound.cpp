// EXPECT: 0
// `const int c(2); int d[c] = { 0, 0 };` — N4659 §8.6 [expr.const]:
// `c` is a constant expression because it was initialised with a
// constant. cc treats the array bound as constant and accepts the
// init-list. Sea-front's const-int fold ran only for the copy-init
// form (`const T name = literal_expr;`), not the equivalent direct-
// init paren form (`const T name(literal_expr);`) — the latter
// parses into ctor_args, not init. The array bound stayed as a
// runtime symbol `c`, so `int d[c] = {0, 0}` lowered as a VLA
// init, which cc rejects.
//
// Extend the fold to also read ctor_args[0] when has_ctor_init
// && ctor_nargs == 1 (the paren-form direct-init shape).
//
// Reduced from g++.dg/template/init8.C.

extern "C" void abort();

int f() {
    const int c(2);
    int d[c] = { 0, 0 };
    return d[0] + (int)sizeof(d);
}

int main() {
    if (f() != 2 * (int)sizeof(int)) abort();
    return 0;
}
