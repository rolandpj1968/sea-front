// EXPECT: 42
// Bare-ident call to a single-overload function template — N4659
// §17.8.2.1 [temp.deduct.call]. No explicit template args, no
// overload set (only one declaration of `unwrap` exists), so the
// callee resolves at parse/sema time as ND_IDENT pointing at an
// ENTITY_TEMPLATE. Without the single-template-candidate rewrite in
// visit_call, the template is never instantiated and the link step
// reports `unwrap_p_..._pe_` undefined.
//
// gcc 4.8 vec.h has hundreds of these (vec_alloc, vec_safe_length,
// vec_free, etc.); this is the minimal reproduction.

template<typename T>
T unwrap(T *p) { return *p; }

int main() {
    int x = 42;
    return unwrap(&x);
}
