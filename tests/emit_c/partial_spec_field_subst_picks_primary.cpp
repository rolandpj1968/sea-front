// EXPECT: 7
// When sema substitutes class-template parameters into a partial-spec
// member's type, the SubstMap must be keyed against the PRIMARY
// template's parameter names — not whichever specialization happened
// to be returned first by name lookup.
//
// vec.h has multiple `vec` specializations:
//   template<typename T, typename A=va_heap, typename L=...> struct vec
//   template<typename T, typename A> struct vec<T, A, vl_embed>
//   template<typename T, typename A> struct vec<T, A, vl_ptr>
//   template<typename T> struct vec<T, va_gc, vl_ptr>     // <-- 1 named param!
//
// If sema's helper picks the 1-param specialization for the SubstMap
// when handling `vec<loop_ptr, va_stack, vl_ptr>`, only T binds; A and
// L are unbound and TY_DEPENDENT('A') / TY_DEPENDENT('L') leak through
// subst_type into the result. The mangled receiver type comes out as
// `sf__vec_t_loop_ptr_A_vl_embed_te_` — the literal `_A_` placeholder
// in the symbol breaks linking.
//
// Drove the gcc 4.8 va_stack::alloc family of cc1plus link errors.
// Fix: select the primary (no template_id_node on inner type, most
// named params). N4659 §17.6.4/9 [temp.arg.default].

struct vl_embed { };
struct vl_ptr { };
struct va_heap { typedef vl_ptr default_layout; };
struct va_gc   { typedef vl_ptr default_layout; };

template<typename T,
         typename A = va_heap,
         typename L = typename A::default_layout>
struct vec { };

template<typename T, typename A>
struct vec<T, A, vl_ptr> {
    T *vec_;
};

// Decoy: 1-param partial spec — if sema's helper picks this when
// resolving members on `vec<X, Y, Z>`, only T binds and A leaks.
template<typename T>
struct vec<T, va_gc, vl_ptr> {
    T *gc_only;
};

template<typename T>
T grab(T *p) { return *p; }

int main() {
    int x = 7;
    vec<int, va_heap, vl_ptr> v = { &x };
    return grab(v.vec_);  // resolving v.vec_ must bind T=int, A=va_heap, L=vl_ptr
                          // — not stop at T after picking the va_gc spec.
}
