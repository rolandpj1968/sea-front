// EXPECT: 7
// A call to a CONST method of the same template class, from inside an
// out-of-class (partial-specialization) method body, previously emitted
// without the '_const' mangling suffix — but the defined symbol DID
// carry '_const', so the emitted C failed to link. Root cause: the
// receiver Type inside a template-instantiated method body often has
// class_def=NULL (a cloned/substituted Type copy), so
// collect_overload_candidates found no candidates, resolve_overload
// returned -1, and the shortcut path's method_is_const() also saw a
// NULL class_def and returned false (no '_const' appended).
//
// Fix: fall back to a TU-wide lookup by (tag, template_args) using
// structural equivalence on the args (the instantiation pass can
// produce distinct Type* for the same concrete type).
//
// Pattern from gcc 4.8 vec.h — vec<T,A,vl_embed>::splice's body calls
// src.length() where length is const-only. N4659 §17.8.1 [temp.inst],
// §10.1.7.1 [dcl.type.cv].
struct vl_embed {};
struct va_heap {};

template<typename T, typename A = va_heap, typename L = vl_embed>
struct vec;

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    int len_;
    int length() const { return len_; }
    void splice(vec& src);
};

template<typename T, typename A>
inline void
vec<T, A, vl_embed>::splice(vec<T, A, vl_embed>& src) {
    // Must mangle with _const — length is a const-only method.
    len_ = src.length();
}

int main() {
    vec<int, va_heap, vl_embed> a;
    vec<int, va_heap, vl_embed> b;
    a.len_ = 0;
    b.len_ = 7;
    a.splice(b);
    return a.len_;
}
