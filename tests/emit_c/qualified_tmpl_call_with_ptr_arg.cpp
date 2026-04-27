// EXPECT: 0
// Qualified template-call site like 'vec<T, A, vl_embed>::method(...)'
// inside a cloned body where T is a pointer type. The
// emit_template_id_suffix path used to have its own type-encode
// switch that emitted TY_PTR as just 'ptr' without recursing into
// the base — losing T's element type. Result: the call mangled to
// 'sf__vec_t_ptr_..._te_::method' instead of the correct
// 'sf__vec_t_rtx_def_ptr_..._te_::method', so the call symbol
// didn't match the (correctly-mangled) declaration's symbol and the
// link failed.
//
// Fix: defer to mangle.c's emit_type_for_mangle, the canonical
// recursive type encoder. Surfaced by gcc 4.8 vec.h's
// 'vec<T, A, vl_embed>::embedded_size(alloc)' inside cloned
// va_gc::reserve / va_heap::reserve bodies.
template<typename T, typename A, typename L> struct vec;

struct va_heap {};
struct vl_embed {};

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    static unsigned embedded_size(unsigned u) { return u + sizeof(T); }
};

template<typename T>
unsigned use_embedded(unsigned u) {
    return vec<T, va_heap, vl_embed>::embedded_size(u);
}

struct rtx_def { int x; };

int main() {
    unsigned r = use_embedded<rtx_def *>(7);
    return r == (7 + sizeof(rtx_def *)) ? 0 : 1;
}
