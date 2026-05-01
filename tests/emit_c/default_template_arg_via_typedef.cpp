// EXPECT: 7
// vec.h-style default-arg expansion: vec<X, A> defaults its third
// arg to typename A::default_layout. With A=va_gc, default is
// vl_embed; gt_pch_nx<T,A>(vec<T,A,vl_embed>*) should match.

struct vl_embed {};
struct vl_ptr {};
struct va_gc {
    typedef vl_embed default_layout;
};
struct va_heap {
    typedef vl_ptr default_layout;
};

template<typename T, typename A = va_heap, typename L = typename A::default_layout>
struct vec {};

// Only vl_embed overload — must match vec<X, va_gc>'s defaulted form.
template<typename T, typename A>
int gt_pch_nx(vec<T, A, vl_embed> *p) { (void)p; return 7; }

struct alias_pair {};

int main() {
    vec<alias_pair, va_gc> v;
    return gt_pch_nx(&v);
}
