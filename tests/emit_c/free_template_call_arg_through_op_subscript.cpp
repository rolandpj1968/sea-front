// EXPECT: 7
// Free function-template call whose argument is a chain through
// `obj[i]` — i.e. operator[] on a class-template instance, then a
// field access. Mirrors gcc 4.8 ipa-cp.c `vec_free (known_aggs[i].items)`.
//
// vec.h has an empty primary `template<class,class,class> struct vec { };`
// and a populated partial specialization with operator[]. visit_subscript
// must (a) find operator[] on a partial spec when the canonical class_def
// misses it, AND (b) substitute the call-site template-id args into the
// returned T& so the chain end is a concrete struct, not TY_DEPENDENT.
// Without (a)+(b) the field-access dead-ends at NULL, the wrapping free
// function template's overload resolution fails to deduce, the callee
// stays ND_IDENT, and codegen emits a bare unmangled name — link breaks.
//
// N4659 §16.5 [over.oper] / §17.8.3.2 [temp.class.spec.match].

struct vl_embed { };
struct vl_ptr { };
struct va_heap { typedef vl_ptr default_layout; };

template<typename T,
         typename A = va_heap,
         typename L = typename A::default_layout>
struct vec { };

template<typename T, typename A>
struct vec<T, A, vl_ptr> {
    T *data_;
    int len_;
    T &operator[](int i) { return data_[i]; }
};

struct entry { int *xs; };

template<typename T>
T grab(T *p) { return *p; }

int main() {
    int x = 7;
    entry e = { &x };
    entry e_arr[1] = { e };
    vec<entry, va_heap, vl_ptr> v = { e_arr, 1 };
    return grab(v[0].xs);
}
