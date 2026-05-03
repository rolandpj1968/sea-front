// EXPECT: 42
// Reduces gcc 4.8 vec.h's safe_splice/splice pattern. A class template
// partial specialization has TWO methods with overloaded shape, both
// defined OOL with the outer 'template<T,A>' head re-stating the
// class's own template params. The methods are NOT free function
// templates — they're members of the partial spec.
//
// safe_splice's body calls splice unqualifiedly. Sema's overload
// resolution at the call site sees splice's name with multiple
// candidates (the in-class declaration and possibly the OOL definition,
// which carries an ND_TEMPLATE_DECL wrapper). Without filtering the
// OOL definitions out of free-fn-template synthesis,
// build_template_id_from_deduced converts the ident to ND_TEMPLATE_ID
// and the instantiation pipeline mangles the call as
// 'splice_t_<args>_te__p_<params>_pe_(src)' — no class scope, no
// implicit-this — and emits a bogus matching definition with the
// class prefix carrying TY_DEPENDENT outer args.
//
// Standard: N4659 §17.5.2/2 [temp.mem] — a member of a class template
// defined outside its class is a member, not a separate template.

template<typename T, typename A, typename L>
struct vec;

struct va_heap_tag {};
struct vl_ptr_tag {};
struct va_gc_tag {};

template<typename T, typename A>
struct vec<T, A, vl_ptr_tag> {
    int v_;
    void splice(vec<T, A, vl_ptr_tag>& src);
    void safe_splice(vec<T, A, vl_ptr_tag>& src);
};

/* The empty more-specialized partial spec — exists in vec.h as
 * vec<T, va_gc, vl_ptr>. Tests that OOL methods of the GENERAL
 * partial spec don't bleed into this one. */
template<typename T>
struct vec<T, va_gc_tag, vl_ptr_tag> {
};

template<typename T, typename A>
void vec<T, A, vl_ptr_tag>::splice(vec<T, A, vl_ptr_tag>& src) {
    v_ = v_ + src.v_;
}

template<typename T, typename A>
void vec<T, A, vl_ptr_tag>::safe_splice(vec<T, A, vl_ptr_tag>& src) {
    splice(src);
}

int main() {
    vec<int, va_heap_tag, vl_ptr_tag> a;
    a.v_ = 20;
    vec<int, va_heap_tag, vl_ptr_tag> b;
    b.v_ = 22;
    a.safe_splice(b);
    return a.v_;
}
