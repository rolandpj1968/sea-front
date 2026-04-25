// EXPECT: 50
// Bare-ident call to a function template with distinct deduced
// types — exercises the single-template-candidate rewrite PLUS
// per-call dedup. Without the rewrite, neither instantiation gets
// emitted. With the rewrite, each deduction produces a distinct
// mangled symbol via the standard ND_TEMPLATE_ID pipeline.
//
// Use pointer-typed args because integer literal suffixes (25L)
// don't currently propagate a long type to the call-site arg, so
// `identity(25L)` would deduce to int. Pointer types preserve their
// element type through deduction (deduce_from_pair recurses into
// TY_PTR base).

template<typename T>
T deref(T *p) { return *p; }

int main() {
    int  i = 20;
    char c = 30;
    int  ri = deref(&i);   // T=int  → deref_t_int_te_
    char rc = deref(&c);   // T=char → deref_t_char_te_
    return ri + (int)rc;   // 20 + 30 = 50
}
