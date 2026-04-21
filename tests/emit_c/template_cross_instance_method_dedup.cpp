// EXPECT: 0
// Cross-instance dedup for template-instantiated method bodies.
//
// The instantiation pass can produce two distinct ND_CLASS_DEF nodes
// for the same logical instantiation when it's discovered via
// multiple paths (e.g. once as a direct use and once as a nested
// template argument). The method-phase dedup used a pointer-set on
// Node* identity, so distinct Nodes pointing at the same logical
// class both emitted their method bodies and produced
// 'redefinition of sf__...__length_p_void_pe__const' inside a single
// TU (weak linkage only helps cross-TU, not within-TU).
//
// Fix: dedup method-phase by (tag, template_args) with structural arg
// comparison — mirrors the struct-phase dedup. Pattern from gcc 4.8
// vec.h's partial specializations of vec<T,A,vl_embed> where the
// same instantiation is reached via the primary + specialization +
// nested-use paths. N4659 §17.8.1 [temp.inst].
struct vl_embed {};
struct vl_ptr {};
struct va_heap {};

template<typename T, typename A = va_heap, typename L = vl_embed>
struct vec;

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    unsigned num_;
    unsigned length() const { return num_; }
    bool is_empty() const { return num_ == 0; }
};

template<typename T, typename A>
struct vec<T, A, vl_ptr> {
    vec<T, A, vl_embed> *vec_;
    unsigned length() const { return vec_ ? vec_->length() : 0; }
};

// Force multiple reachability paths for vec<int,va_heap,vl_embed>:
// 1. Direct global variable
// 2. Member of vec<int,va_heap,vl_ptr>'s method body
// 3. Local variable in main
vec<int, va_heap, vl_embed> g;

int main() {
    vec<int, va_heap, vl_ptr> p;
    vec<int, va_heap, vl_embed> e;
    return p.length() + e.length() + g.length();
}
