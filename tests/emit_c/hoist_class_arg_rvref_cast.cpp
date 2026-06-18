// EXPECT: 0
// `f(static_cast<S&&>(y))` where f takes S by value triggers
// hoist_pass_by_value_copy — emit `S __sf_cpy; copy_ctor(&__sf_cpy,
// &(arg));`. arg here is the rvref cast, which sea-front lowers as
// `(S *)&(y)` — already a pointer. The surrounding `&(...)` then
// becomes `&((S *)&(y))` = T**, which the copy ctor rejects.
//
// Forward the cast's inner operand so the surrounding `&(...)`
// yields the source's lvalue address. Mirrors the same fix already
// in emit_arg_for_param's stmt-expr path. N4659 §8.2.10
// [expr.static.cast] + §16.3.3.1.4 [over.match.ref].
//
// Reduced from g++.dg/cpp0x/rv9p.C.

extern "C" void abort();

struct S {
    S(): i(2) {}
    S(const S &s): i(s.i) {}
    int i;
};

void f(S x) { x.i = 0; }

int main() {
    S y;
    f(static_cast<S &&>(y));
    if (y.i != 2) abort();
    return 0;
}
