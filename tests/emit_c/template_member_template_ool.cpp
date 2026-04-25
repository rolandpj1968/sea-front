// EXPECT: 0
// Test: in-class declaration of a static member template, defined OUT-OF-LINE.
// Pattern: gcc 4.8 vec.h va_heap::reserve / va_heap::release.
// N4659 §17.5.2 [temp.mem] — a member template can be declared in-class
// and defined out-of-class. The instantiation pass must find the OOL
// definition when the in-class member is just a declaration.
//
// Previously broken: the parser emits in-class function declarations as
// ND_VAR_DECL with TY_FUNC, but the OOL-search predicate only matched
// ND_FUNC_DECL — so no OOL search fired and no body was emitted, leaving
// every va_heap::reserve / release call unresolved at link time.
struct va_heap {
    template<typename T>
    static void reserve(T*& v, int n);
    template<typename T>
    static void release(T*& v);
};

template<typename T>
void va_heap::reserve(T*& v, int n) { (void)v; (void)n; }

template<typename T>
void va_heap::release(T*& v) { v = 0; }

struct vl_embed {};
struct vl_ptr {};

template<typename T, typename A, typename L>
struct vec;

template<typename T, typename A>
struct vec<T, A, vl_ptr> {
    vec<T, A, vl_embed> *vec_;
    void reserve(int n) { A::reserve(vec_, n); }
    void release()      { A::release(vec_); }
};

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    int n;
    T data[1];
};

int main() {
    vec<int, va_heap, vl_ptr> v;
    v.vec_ = 0;
    v.reserve(8);
    v.release();
    return v.vec_ == 0 ? 0 : 1;
}
