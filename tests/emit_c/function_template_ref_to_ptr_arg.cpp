// EXPECT: 13
// Bare-ident function template whose parameter is reference-to-pointer
// to a class-template instance — the gcc 4.8 vec.h shape:
//
//   template<typename T> T grab(Holder<T> *&h) { return h->take(); }
//
// In C++, `h` is a reference to a pointer; `h->take()` works because
// the reference auto-binds. Sea-front lowers `T*&` to `T**` in C, so
// the body must emit `(*h)->take()` (auto-dereference the reference).
// AND the method call must dispatch through the mangled Holder<T>::take
// symbol, not be left as `(*h)->take()` (invalid C).
//
// This is the exact shape of vec_safe_push, vec_safe_grow_cleared,
// etc. — which is why ~600 vec.h calls fail to link.

template<typename T>
struct Holder {
    T value;
    T take() { return value; }
};

template<typename T>
T grab(Holder<T> *&h) { return h->take(); }

int main() {
    Holder<int> hi;
    hi.value = 13;
    Holder<int> *p = &hi;
    return grab(p);   // T=int deduced; h is Holder<int>*&; body uses h->take()
}
