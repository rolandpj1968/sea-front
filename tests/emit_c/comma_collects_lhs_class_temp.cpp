// EXPECT: 1
// The dep-collector walks ND_COMMA via the .binary union member
// (n->binary.lhs/rhs), but ND_COMMA's struct layout is
// {Node *lhs, Node *rhs} — no leading op like ND_BINARY. So
// reading via .binary skipped the LHS at the wrong offset and
// the LHS of a comma expression went uncollected. Sema's
// visit_comma had the same fix already; the collector was the
// remaining site.
//
// Reduced shape: `(SideEffect<1>(), SideEffect<2>())` — the LHS
// `SideEffect<1>()` is a class-template type-call that must
// instantiate SideEffect<1> for its ctor side effect even
// though the comma value is discarded. Without the fix only
// SideEffect<2> got instantiated and the hoist emitted a
// question-mark-typed `int __SF_temp_N = T();` placeholder
// that wouldn't link.
//
// Plus a class-temp-hoist fix in emit_c.c: the ND_TEMPLATE_ID
// type-call probe now wraps NTTP literal args as TY_NTTP_VALUE
// so find_class_def_by_tag_args matches the instantiated
// class_def — covers both `A<1>()` and `A<2>()` symmetrically.

extern "C" void abort();

static int order[2];
static int pos = 0;

template<int I> struct SideEffect {
    SideEffect() { if (pos < 2) order[pos++] = I; }
    int value() const { return I; }
};

int main() {
    /* The LHS SideEffect<1>() runs for its ctor side effect.
     * The RHS SideEffect<2>() is the comma's value; we then
     * call .value() on it to confirm RHS == 2. */
    SideEffect<2> v = (SideEffect<1>(), SideEffect<2>());
    if (v.value() != 2) abort();
    /* The LHS ctor ran first per N4659 §8.19 [expr.comma]. */
    if (pos != 2 || order[0] != 1 || order[1] != 2) abort();
    return 1;  /* harness reads non-zero as pass too — keep stable */
}
