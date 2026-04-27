// EXPECT: 0
// In a cloned template body, a call like 'A::reserve(v, ...)' where A
// is a template parameter that resolved to a class with a member
// template (gcc 4.8 vec.h pattern) — sema cannot resolve the member-
// template name, so callee_ft is NULL and emit_arg_for_param fell
// through to emit_expr(arg) for each arg. For ND_IDENT v naming a
// ref-param, that emits '(*v)' (the standard ref-deref) — passing the
// dereferenced pointer value into a function expecting vec** → NULL
// dereferenced inside va_heap::reserve, segfault.
//
// Fix: when the arg is a ref-param ident AND the param type is
// either known-ref OR unknown (NULL), suppress the deref and pass
// the bare ident through. The C-level value of v is already the
// address (T**); ref-to-ref pass-through is the dominant pattern in
// member-template calls, and the alternative would have been wrong
// anyway (passing a deref to a ref param is a type mismatch warning
// in C, an outright segfault at runtime if the ref's underlying
// pointer is NULL).
//
// Pattern: gcc 4.8 vec.h vec_safe_reserve calls A::reserve(v, ...)
// inside a cloned vec_safe_reserve<T,A> body — emitted as
// 'sf__va_heap__reserve(*v, ...)' before the fix, segfaulting
// genautomata at runtime. N4659 §11.3.2 [dcl.ref].
struct vl_embed {};
struct va_heap {
    template<typename T>
    static void reserve(T*& v, unsigned n, bool exact);
};

template<typename T>
void va_heap::reserve(T*& v, unsigned n, bool exact) {
    v = (T*)0x1234;
    (void)n;
    (void)exact;
}

template<typename T, typename A, typename L> struct vec;

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    int data;
};

template<typename T, typename A>
inline bool vec_safe_reserve(vec<T, A, vl_embed>*& v, unsigned nelems, bool exact = false) {
    bool extend = nelems ? true : false;
    if (extend)
        A::reserve(v, nelems, exact);
    return extend;
}

int main() {
    vec<int, va_heap, vl_embed> *p = 0;
    vec_safe_reserve<int, va_heap>(p, 5);
    return p == (vec<int, va_heap, vl_embed>*)0x1234 ? 0 : 1;
}
