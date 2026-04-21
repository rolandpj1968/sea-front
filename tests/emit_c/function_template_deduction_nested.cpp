// EXPECT: 1
// Pattern from gcc 4.8 is-a.h: a wrapper function template (dyn_cast)
// whose body calls another function template (is_a) that deduces U
// from its call arg. When dyn_cast<T> is instantiated, the cloned
// body contains 'is_a<T>(p)' which must ALSO be instantiated and
// rewritten with the full (explicit + deduced) arg set.
//
// The bug: instantiation's deduction path built a SYNTHETIC
// template_id Node carrying explicit+deduced args and swapped
// req->template_id to it. The downstream rewrite (ND_TEMPLATE_ID →
// ND_IDENT with mangled name) then fired on the SYNTHETIC, leaving
// the real in-tree node with its original ND_TEMPLATE_ID kind and
// 1-arg args list. At emit time that node either fell through to the
// emit_expr default ('/* expr */') or got a wrong, shorter mangled
// name missing the deduced arg — producing a link-time undefined
// reference.
//
// Fix: extend req->template_id's args in place instead of swapping.
// Same Node identity, so the downstream rewrite updates the real
// in-tree node and emit sees ND_IDENT with the correct full-args
// mangled name.
//
// N4659 §17.8.2.1 [temp.deduct.call].
template <typename T, typename U>
inline bool is_a(U *p) {
    return p && *p == 42;
}

int main() {
    int x = 42;
    // Explicit T=bool + deduced U=int. The instantiated is_a's
    // mangled name must carry BOTH in the suffix; otherwise the
    // call site disagrees with the definition and the C compiler
    // either emits an implicit decl (ordering) or fails with a
    // conflicting-types error (definition emitted first).
    return is_a<bool>(&x) ? 1 : 0;
}
